/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <bluetooth/mesh/models.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>

#include "audio_player.h"
#include "chat_cli.h"
#include "model_handler.h"
#include "sd_card.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(chat);

static const struct shell *chat_shell;

/******************************************************************************/
/*************************** Health server setup ******************************/
/******************************************************************************/
static struct bt_mesh_health_srv health_srv;

BT_MESH_HEALTH_PUB_DEFINE(health_pub, 0);

/******************************************************************************/
/***************************** Chat model setup *******************************/
/******************************************************************************/
struct presence_cache {
	uint16_t addr;
	enum bt_mesh_chat_cli_presence presence;
};

/* Cache of Presence values of other chat clients. */
static struct presence_cache presence_cache[
			    CONFIG_BT_MESH_CHAT_SAMPLE_PRESENCE_CACHE_SIZE];

static const uint8_t *presence_string[] = {
	[BT_MESH_CHAT_CLI_PRESENCE_AVAILABLE] = "available",
	[BT_MESH_CHAT_CLI_PRESENCE_AWAY] = "away",
	[BT_MESH_CHAT_CLI_PRESENCE_DO_NOT_DISTURB] = "dnd",
	[BT_MESH_CHAT_CLI_PRESENCE_INACTIVE] = "inactive",
};

/**
 * Returns true if the specified address is an address of the local element.
 */
static bool address_is_local(const struct bt_mesh_model *mod, uint16_t addr)
{
	return bt_mesh_model_elem(mod)->rt->addr == addr;
}

/**
 * Returns true if the provided address is unicast address.
 */
static bool address_is_unicast(uint16_t addr)
{
	return (addr > 0) && (addr <= 0x7FFF);
}

/**
 * Returns true if the node is new or the presence status is different from
 * the one stored in the cache.
 */
static bool presence_cache_entry_check_and_update(uint16_t addr,
				       enum bt_mesh_chat_cli_presence presence)
{
	static size_t presence_cache_head;
	size_t i;

	/* Find address in cache. */
	for (i = 0; i < ARRAY_SIZE(presence_cache); i++) {
		if (presence_cache[i].addr == addr) {
			if (presence_cache[i].presence == presence) {
				return false;
			}

			/* Break since the node in the cache. */
			break;
		}
	}

	/* Not in cache. */
	if (i == ARRAY_SIZE(presence_cache)) {
		for (i = 0; i < ARRAY_SIZE(presence_cache); i++) {
			if (!presence_cache[i].addr) {
				break;
			}
		}

		/* Cache is full. */
		if (i == ARRAY_SIZE(presence_cache)) {
			i = presence_cache_head;
			presence_cache_head = (presence_cache_head + 1)
			% CONFIG_BT_MESH_CHAT_SAMPLE_PRESENCE_CACHE_SIZE;
		}
	}

	/* Update cache. */
	presence_cache[i].addr = addr;
	presence_cache[i].presence = presence;

	return true;
}

static void print_client_status(void);

static void handle_chat_start(struct bt_mesh_chat_cli *chat)
{
	print_client_status();
}

/* Deferred worker for "list": sd_card_list_playable_files() does a full
 * FAT directory scan over SPI, which is too slow to run directly on the
 * Bluetooth Mesh model's incoming-message dispatch path -- while it's
 * running, this node can't process other concurrently-arriving segmented
 * messages (see the mesh RX segment slot exhaustion this caused). Mirrors
 * audio_player.c's own thread-plus-semaphore pattern for the same class
 * of problem, with its own stack for the same reason: FatFs keeps
 * per-call work buffers on the stack.
 */
#define LIST_WORKER_STACK_SIZE 4096
/* One priority step below audio_player.c's AUDIO_THREAD_PRIO (7), so a
 * list scan can never delay audio buffer feeding.
 */
#define LIST_WORKER_THREAD_PRIO 8

K_THREAD_STACK_DEFINE(list_worker_stack, LIST_WORKER_STACK_SIZE);

static struct {
	struct k_sem request;
	struct k_mutex lock;
	struct k_thread thread;
	k_tid_t thread_id;
	bool busy;
	struct bt_mesh_chat_cli *pending_chat;
	struct bt_mesh_msg_ctx pending_ctx;
} list_worker;

static void list_worker_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		struct bt_mesh_chat_cli *chat;
		struct bt_mesh_msg_ctx ctx;
		char files[BT_MESH_CHAT_CLI_MSG_MAXLEN_MESSAGE];
		int count;

		k_sem_take(&list_worker.request, K_FOREVER);

		k_mutex_lock(&list_worker.lock, K_FOREVER);
		chat = list_worker.pending_chat;
		ctx = list_worker.pending_ctx;
		k_mutex_unlock(&list_worker.lock);

		count = sd_card_list_playable_files(files, sizeof(files));

		(void)bt_mesh_chat_cli_reply_send(chat, &ctx,
						  count > 0 ? (const uint8_t *)files
							    : (const uint8_t *)"(none)");

		k_mutex_lock(&list_worker.lock, K_FOREVER);
		list_worker.busy = false;
		k_mutex_unlock(&list_worker.lock);
	}
}

static int list_worker_request(struct bt_mesh_chat_cli *chat, struct bt_mesh_msg_ctx *ctx)
{
	k_mutex_lock(&list_worker.lock, K_FOREVER);
	if (list_worker.busy) {
		k_mutex_unlock(&list_worker.lock);
		return -EALREADY;
	}

	list_worker.busy = true;
	list_worker.pending_chat = chat;
	list_worker.pending_ctx = *ctx;
	k_mutex_unlock(&list_worker.lock);

	k_sem_give(&list_worker.request);
	return 0;
}

static void list_worker_init(void)
{
	k_sem_init(&list_worker.request, 0, 1);
	k_mutex_init(&list_worker.lock);

	list_worker.thread_id = k_thread_create(&list_worker.thread, list_worker_stack,
						K_THREAD_STACK_SIZEOF(list_worker_stack),
						list_worker_thread, NULL, NULL, NULL,
						K_PRIO_PREEMPT(LIST_WORKER_THREAD_PRIO), 0,
						K_NO_WAIT);
	(void)k_thread_name_set(list_worker.thread_id, "list_worker");
}

/**
 * Mesh messages use a formal "cmd args" grammar, e.g. "play a.wav". This
 * keeps free-form chat text (which never matches a known verb) inert, same
 * as before, while giving each action its own explicit command instead of
 * treating every message as a filename to try to play.
 */
static void cmd_play(const char *args)
{
	int err;

	err = audio_player_play_file(args);
	if (err && err != -ENOENT) {
		LOG_WRN("Failed to play %s: %d", args, err);
	}
}

/**
 * "find <file>" checks whether this node has the file, without playing it.
 * Only a node that has the file replies, so the sender can tell who has it
 * purely from which nodes ack -- this is also the tool for confirming the
 * mesh is actually delivering messages end-to-end across the whole fleet.
 */
static void cmd_find(struct bt_mesh_chat_cli *chat, struct bt_mesh_msg_ctx *ctx,
		     const char *args)
{
	char path[128];
	char reply[64];

	if (sd_card_resolve_root_file(args, path, sizeof(path))) {
		snprintf(reply, sizeof(reply), "found %s", args);
		(void)bt_mesh_chat_cli_reply_send(chat, ctx, (const uint8_t *)reply);
	}
}

/**
 * "list" asks every node to announce its playable files, comma-separated,
 * in one reply -- unlike "find", every node replies (even with "(none)"),
 * since the point is a full roll call rather than locating one file.
 */
static void cmd_list(struct bt_mesh_chat_cli *chat, struct bt_mesh_msg_ctx *ctx)
{
	int err;

	err = list_worker_request(chat, ctx);
	if (err) {
		LOG_WRN("Failed to queue list request: %d", err);
	}
}

static void handle_command(struct bt_mesh_chat_cli *chat, struct bt_mesh_msg_ctx *ctx,
			   const uint8_t *msg)
{
	const char *text = (const char *)msg;
	const char *space = strchr(text, ' ');
	size_t cmd_len = space ? (size_t)(space - text) : strlen(text);
	const char *args = space ? space + 1 : "";

	if (cmd_len == strlen("play") && strncmp(text, "play", cmd_len) == 0) {
		cmd_play(args);
	} else if (cmd_len == strlen("find") && strncmp(text, "find", cmd_len) == 0) {
		cmd_find(chat, ctx, args);
	} else if (cmd_len == strlen("list") && strncmp(text, "list", cmd_len) == 0) {
		cmd_list(chat, ctx);
	}
	/* Unrecognized verb (or plain chat text): no action, same as before. */
}

static void handle_chat_presence(struct bt_mesh_chat_cli *chat,
				 struct bt_mesh_msg_ctx *ctx,
				 enum bt_mesh_chat_cli_presence presence)
{
	if (address_is_local(chat->model, ctx->addr)) {
		if (address_is_unicast(ctx->recv_dst)) {
			shell_print(chat_shell, "<you> are %s",
				    presence_string[presence]);
		}
	} else {
		if (address_is_unicast(ctx->recv_dst)) {
			shell_print(chat_shell, "<0x%04X> is %s", ctx->addr,
				    presence_string[presence]);
		} else if (presence_cache_entry_check_and_update(ctx->addr,
								 presence)) {
			shell_print(chat_shell, "<0x%04X> is now %s",
				    ctx->addr,
				    presence_string[presence]);
		}
	}
}

static void handle_chat_message(struct bt_mesh_chat_cli *chat,
				struct bt_mesh_msg_ctx *ctx,
				const uint8_t *msg)
{
	/* Don't react to our own messages. */
	if (address_is_local(chat->model, ctx->addr)) {
		return;
	}

	shell_print(chat_shell, "<0x%04X>: %s", ctx->addr, msg);
	handle_command(chat, ctx, msg);
}

static void handle_chat_private_message(struct bt_mesh_chat_cli *chat,
					struct bt_mesh_msg_ctx *ctx,
					const uint8_t *msg)
{
	/* Don't react to our own messages. */
	if (address_is_local(chat->model, ctx->addr)) {
		return;
	}

	shell_print(chat_shell, "<0x%04X>: *you* %s", ctx->addr, msg);
	handle_command(chat, ctx, msg);
}

static void handle_chat_message_reply(struct bt_mesh_chat_cli *chat,
				      struct bt_mesh_msg_ctx *ctx,
				      const uint8_t *msg)
{
	shell_print(chat_shell, "<0x%04X>: %s", ctx->addr, msg);
}

static const struct bt_mesh_chat_cli_handlers chat_handlers = {
	.start = handle_chat_start,
	.presence = handle_chat_presence,
	.message = handle_chat_message,
	.private_message = handle_chat_private_message,
	.message_reply = handle_chat_message_reply,
};

/* .. include_startingpoint_model_handler_rst_1 */
static struct bt_mesh_chat_cli chat = {
	.handlers = &chat_handlers,
};

static struct bt_mesh_cfg_cli cfg_cli = {};

static struct bt_mesh_elem elements[] = {
	BT_MESH_ELEM(
		1,
		BT_MESH_MODEL_LIST(
			BT_MESH_MODEL_CFG_SRV,
			BT_MESH_MODEL_CFG_CLI(&cfg_cli),
			BT_MESH_MODEL_HEALTH_SRV(&health_srv, &health_pub)),
		BT_MESH_MODEL_LIST(BT_MESH_MODEL_CHAT_CLI(&chat))),
};
/* .. include_endpoint_model_handler_rst_1 */

static void print_client_status(void)
{
	if (!bt_mesh_is_provisioned()) {
		shell_print(chat_shell,
			    "The mesh node is not provisioned. Please provision the mesh node before using the chat.");
	} else {
		shell_print(chat_shell,
			    "The mesh node is provisioned. The client address is 0x%04x.",
			    bt_mesh_model_elem(chat.model)->rt->addr);
	}

	shell_print(chat_shell, "Current presence: %s",
		    presence_string[chat.presence]);
}

static const struct bt_mesh_comp comp = {
	.cid = CONFIG_BT_COMPANY_ID,
	.elem = elements,
	.elem_count = ARRAY_SIZE(elements),
};

/******************************************************************************/
/******************************** Chat shell **********************************/
/******************************************************************************/
static int cmd_status(const struct shell *shell, size_t argc, char *argv[])
{
	print_client_status();

	return 0;
}

static int cmd_message(const struct shell *shell, size_t argc, char *argv[])
{
	int err;

	if (argc < 2) {
		return -EINVAL;
	}

	err = bt_mesh_chat_cli_message_send(&chat, argv[1]);
	if (err) {
		LOG_WRN("Failed to send message: %d", err);
	}

	/* Print own messages in the chat. */
	shell_print(shell, "<you>: %s", argv[1]);

	return 0;
}

static int cmd_private_message(const struct shell *shell, size_t argc,
			       char *argv[])
{
	uint16_t addr;
	int err;

	if (argc < 3) {
		return -EINVAL;
	}

	addr = strtol(argv[1], NULL, 0);

	/* Print own message to the chat. */
	shell_print(shell, "<you>: *0x%04X* %s", addr, argv[2]);

	err = bt_mesh_chat_cli_private_message_send(&chat, addr, argv[2]);
	if (err) {
		LOG_WRN("Failed to publish message: %d", err);
	}

	return 0;
}

static int cmd_presence_set(const struct shell *shell, size_t argc,
			    char *argv[])
{
	size_t i;

	if (argc < 2) {
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(presence_string); i++) {
		if (!strcmp(argv[1], presence_string[i])) {
			enum bt_mesh_chat_cli_presence presence;
			int err;

			presence = i;

			err = bt_mesh_chat_cli_presence_set(&chat, presence);
			if (err) {
				LOG_WRN("Failed to update presence: %d", err);
			}

			/* Print own presence in the chat. */
			shell_print(shell, "You are now %s",
				    presence_string[presence]);

			return 0;
		}
	}

	shell_print(shell,
		    "Unknown presence status: %s. Possible presence statuses:",
		    argv[1]);
	for (i = 0; i < ARRAY_SIZE(presence_string); i++) {
		shell_print(shell, "%s", presence_string[i]);
	}

	return 0;
}

static int cmd_presence_get(const struct shell *shell, size_t argc,
			    char *argv[])
{
	uint16_t addr;
	int err;

	if (argc < 2) {
		return -EINVAL;
	}

	addr = strtol(argv[1], NULL, 0);

	err = bt_mesh_chat_cli_presence_get(&chat, addr);
	if (err) {
		LOG_WRN("Failed to publish message: %d", err);
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(presence_cmds,
	SHELL_CMD_ARG(set, NULL,
		      "Set presence of the current client <presence: available, away, dnd or inactive>",
		      cmd_presence_set, 2, 0),
	SHELL_CMD_ARG(get, NULL,
		      "Get presence status of the remote node <node>",
		      cmd_presence_get, 2, 0),
	SHELL_SUBCMD_SET_END
);

static int cmd_presence(const struct shell *shell, size_t argc, char *argv[])
{
	if (argc == 1) {
		shell_help(shell);
		/* shell returns 1 when help is printed */
		return 1;
	}

	if (argc != 3) {
		return -EINVAL;
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(chat_cmds,
	SHELL_CMD_ARG(status, NULL, "Print client status", cmd_status, 1, 0),
	SHELL_CMD(presence, &presence_cmds, "Presence commands", cmd_presence),
	/* SHELL_OPT_ARG_RAW: <cmd> <args> is a multi-word command (e.g.
	 * "play a.wav"), so everything after <node>/the subcommand name must
	 * be passed through as one raw, unsplit string instead of being
	 * tokenized on whitespace.
	 */
	SHELL_CMD_ARG(private, NULL,
		      "Send a command as a private message to a client <node> <cmd> <args>",
		      cmd_private_message, 2, SHELL_OPT_ARG_RAW),
	SHELL_CMD_ARG(msg, NULL, "Send a command to the chat <cmd> <args>",
		      cmd_message, 1, SHELL_OPT_ARG_RAW),
	SHELL_SUBCMD_SET_END
);

static int cmd_chat(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 1) {
		shell_help(shell);
		/* shell returns 1 when help is printed */
		return 1;
	}

	shell_error(shell, "%s unknown parameter: %s", argv[0], argv[1]);

	return -EINVAL;
}

SHELL_CMD_ARG_REGISTER(chat, &chat_cmds, "Bluetooth Mesh Chat Client commands",
		       cmd_chat, 1, 1);

/******************************************************************************/
/******************************** Cue shell ************************************/
/******************************************************************************/
/*
 * Thin, discoverable wrappers around the "cmd args" mesh grammar: `cue play
 * <file>` / `cue find <file>` are shorthand for `chat msg play <file>` /
 * `chat msg find <file>` (always a group broadcast). `chat msg`/`chat
 * private` remain available for free-form messages or for targeting one
 * node directly.
 */
static int cue_send(const struct shell *shell, const char *cmd, const char *args)
{
	char msg[BT_MESH_CHAT_CLI_MSG_MAXLEN_MESSAGE];
	int err;

	if (args[0] != '\0') {
		snprintf(msg, sizeof(msg), "%s %s", cmd, args);
	} else {
		snprintf(msg, sizeof(msg), "%s", cmd);
	}

	err = bt_mesh_chat_cli_message_send(&chat, (const uint8_t *)msg);
	if (err) {
		LOG_WRN("Failed to send message: %d", err);
	}

	shell_print(shell, "<you>: %s", msg);

	return 0;
}

static int cmd_cue_play(const struct shell *shell, size_t argc, char *argv[])
{
	return cue_send(shell, "play", argv[1]);
}

static int cmd_cue_find(const struct shell *shell, size_t argc, char *argv[])
{
	return cue_send(shell, "find", argv[1]);
}

static int cmd_cue_list(const struct shell *shell, size_t argc, char *argv[])
{
	return cue_send(shell, "list", "");
}

SHELL_STATIC_SUBCMD_SET_CREATE(cue_cmds,
	SHELL_CMD_ARG(play, NULL,
		      "Play a file on every mesh node that has it <filename>",
		      cmd_cue_play, 2, 0),
	SHELL_CMD_ARG(find, NULL,
		      "Ask which mesh nodes have a file, without playing it <filename>",
		      cmd_cue_find, 2, 0),
	SHELL_CMD_ARG(list, NULL,
		      "Ask every mesh node to list its playable files",
		      cmd_cue_list, 1, 0),
	SHELL_SUBCMD_SET_END
);

static int cmd_cue(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 1) {
		shell_help(shell);
		/* shell returns 1 when help is printed */
		return 1;
	}

	shell_error(shell, "%s unknown parameter: %s", argv[0], argv[1]);

	return -EINVAL;
}

SHELL_CMD_ARG_REGISTER(cue, &cue_cmds, "Broadcast a play/find command to the mesh",
		       cmd_cue, 1, 1);

/******************************************************************************/
/******************************** Public API **********************************/
/******************************************************************************/
void model_handler_set_pub(uint16_t addr, uint16_t app_key_idx, uint8_t ttl)
{
	if (!chat.model || !chat.model->pub) {
		return;
	}

	chat.model->pub->addr = addr;
	chat.model->pub->key = app_key_idx;
	chat.model->pub->ttl = ttl;
}

const struct bt_mesh_comp *model_handler_init(void)
{
	chat_shell = shell_backend_uart_get_ptr();

	list_worker_init();

	return &comp;
}

uint16_t model_handler_addr(void)
{
	if (!chat.model) {
		return 0;
	}

	return bt_mesh_model_elem(chat.model)->rt->addr;
}
