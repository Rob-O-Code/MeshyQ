/** @file
 *  @brief Structured JSON control protocol for the MeshyQ web app.
 *
 *  Runs entirely on web_api_cdc_acm_uart (see boards, *.overlay), a second
 *  USB CDC-ACM interface separate from the interactive shell/console. The
 *  shell's console shares its byte stream with Zephyr's own logging
 *  (CONFIG_SHELL_LOG_BACKEND) and async shell_print() calls from
 *  model_handler.c, which makes it unsuitable for a client that needs to
 *  reliably parse command responses. This module exists so the web app
 *  never has to scrape that stream: one line in, one line out, always
 *  valid JSON.
 *
 *  Wire protocol: newline-delimited JSON.
 *    Host -> device: {"v":1,"id":<n>,"cmd":"<name>","args":{...}}
 *    Device -> host (response): {"v":1,"id":<n>,"ok":true,"result":{...}}
 *                             or {"v":1,"id":<n>,"ok":false,"error":{"code":"...","message":"..."}}
 *    Device -> host (event, unsolicited): {"v":1,"event":"<name>",...}
 *  "v" is this wire protocol's version, independent of APP_VERSION_STRING;
 *  bump WEB_API_PROTOCOL_VERSION only on breaking shape changes.
 */

#include "web_api.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <app_version.h>
#include <zephyr/data/json.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/usb/usbd.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(web_api, CONFIG_LOG_DEFAULT_LEVEL);

#include "audio_player.h"
#include "model_handler.h"
#include "sd_card.h"

#define WEB_API_PROTOCOL_VERSION 1

static const struct device *const web_api_uart =
	DEVICE_DT_GET(DT_NODELABEL(web_api_cdc_acm_uart));

/******************************************************************************/
/******************************** RX pipeline ********************************/
/******************************************************************************/
#define WEB_API_RX_RINGBUF_SIZE 256
#define WEB_API_THREAD_STACK_SIZE 2560
/* One priority step below model_handler.c's LIST_WORKER_THREAD_PRIO (8), so
 * JSON command handling can never delay a pending mesh "list" scan.
 */
#define WEB_API_THREAD_PRIO 9

RING_BUF_DECLARE(web_api_rx_ringbuf, WEB_API_RX_RINGBUF_SIZE);
K_SEM_DEFINE(web_api_rx_sem, 0, 1);
K_SEM_DEFINE(web_api_dtr_sem, 0, 1);

K_THREAD_STACK_DEFINE(web_api_thread_stack, WEB_API_THREAD_STACK_SIZE);

static struct {
	struct k_thread thread;
	k_tid_t thread_id;
	bool connected;
	char line[CONFIG_WEB_API_LINE_BUF_SIZE];
	size_t line_len;
} web_api;

static void web_api_handle_line(char *line, size_t len);

static void web_api_uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t buf[32];
			int n = uart_fifo_read(dev, buf, sizeof(buf));

			if (n > 0) {
				uint32_t put = ring_buf_put(&web_api_rx_ringbuf, buf, (uint32_t)n);

				if (put < (uint32_t)n) {
					LOG_WRN("RX ring buffer full, dropped %d byte(s)",
						n - (int)put);
				}
				k_sem_give(&web_api_rx_sem);
			}
		}
	}
}

void web_api_usbd_msg_handler(struct usbd_context *ctx, const struct usbd_msg *msg)
{
	ARG_UNUSED(ctx);

	if (msg->type != USBD_MSG_CDC_ACM_CONTROL_LINE_STATE || msg->dev != web_api_uart) {
		return;
	}

	uint32_t dtr = 0;

	uart_line_ctrl_get(msg->dev, UART_LINE_CTRL_DTR, &dtr);
	if (dtr) {
		k_sem_give(&web_api_dtr_sem);
	} else {
		web_api.connected = false;
		/* Wake the thread if it's blocked waiting for RX, so it
		 * notices "connected" went false and loops back to wait for
		 * DTR again instead of blocking forever.
		 */
		k_sem_give(&web_api_rx_sem);
	}
}

static void web_api_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	uart_irq_callback_set(web_api_uart, web_api_uart_isr);

	while (1) {
		uint8_t byte;

		/* Only start pulling bytes off the wire once a client has
		 * actually opened the port; re-armed every time DTR drops
		 * and comes back.
		 */
		k_sem_take(&web_api_dtr_sem, K_FOREVER);
		web_api.connected = true;
		web_api.line_len = 0;
		uart_irq_rx_enable(web_api_uart);

		while (web_api.connected) {
			k_sem_take(&web_api_rx_sem, K_FOREVER);

			while (ring_buf_get(&web_api_rx_ringbuf, &byte, 1) == 1) {
				if (byte == '\n') {
					web_api.line[web_api.line_len] = '\0';
					web_api_handle_line(web_api.line, web_api.line_len);
					web_api.line_len = 0;
				} else if (byte != '\r') {
					if (web_api.line_len <
					    sizeof(web_api.line) - 1) {
						web_api.line[web_api.line_len++] = byte;
					} else {
						/* Line too long for the buffer:
						 * drop it rather than overrun or
						 * silently truncate a command.
						 */
						LOG_WRN("Request line too long, dropping it");
						web_api.line_len = 0;
					}
				}
			}
		}

		uart_irq_rx_disable(web_api_uart);
	}
}

/******************************************************************************/
/****************************** TX / responses ********************************/
/******************************************************************************/
static void web_api_send_line(const char *json, size_t len)
{
	size_t i;

	if (!web_api.connected) {
		return;
	}

	for (i = 0; i < len; i++) {
		uart_poll_out(web_api_uart, (unsigned char)json[i]);
	}
	uart_poll_out(web_api_uart, '\n');
}

/* Escapes `text` into `buf` for embedding as a JSON string body (without the
 * surrounding quotes). `text` may be arbitrary (mesh chat text, filenames);
 * falls back to an empty string if the escaped form doesn't fit, rather than
 * emit broken JSON.
 */
static const char *web_api_escape(char *buf, size_t buf_size, const char *text)
{
	size_t len = strlen(text);

	if (len >= buf_size) {
		len = buf_size - 1;
	}
	memcpy(buf, text, len);
	buf[len] = '\0';

	if (json_escape(buf, &len, buf_size) < 0) {
		buf[0] = '\0';
	}

	return buf;
}

/* Appends a formatted string to buf at *pos, clamping *pos to buf_size on
 * overflow so a later call can't underflow "buf_size - *pos" and write past
 * the end of buf.
 */
static void web_api_append(char *buf, size_t buf_size, size_t *pos, const char *fmt, ...)
{
	va_list args;
	int n;

	if (*pos >= buf_size) {
		return;
	}

	va_start(args, fmt);
	n = vsnprintf(&buf[*pos], buf_size - *pos, fmt, args);
	va_end(args);

	if (n < 0 || (size_t)n >= buf_size - *pos) {
		*pos = buf_size;
	} else {
		*pos += (size_t)n;
	}
}

static void send_result_empty(int32_t id)
{
	char buf[48];
	int len;

	len = snprintf(buf, sizeof(buf), "{\"v\":%d,\"id\":%d,\"ok\":true,\"result\":{}}",
		       WEB_API_PROTOCOL_VERSION, id);
	if (len > 0 && (size_t)len < sizeof(buf)) {
		web_api_send_line(buf, (size_t)len);
	}
}

/* `code`/`message` must always be our own fixed string literals (never
 * mesh/user-originated text) -- they're embedded without escaping.
 */
static void send_error(int32_t id, const char *code, const char *message)
{
	char buf[192];
	int len;

	len = snprintf(buf, sizeof(buf),
		       "{\"v\":%d,\"id\":%d,\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
		       WEB_API_PROTOCOL_VERSION, id, code, message);
	if (len > 0 && (size_t)len < sizeof(buf)) {
		web_api_send_line(buf, (size_t)len);
	}
}

static void send_result_status(int32_t id, bool provisioned, uint16_t addr,
			       const char *presence)
{
	char buf[128];
	int len;

	len = snprintf(buf, sizeof(buf),
		       "{\"v\":%d,\"id\":%d,\"ok\":true,\"result\":"
		       "{\"provisioned\":%s,\"addr\":\"0x%04x\",\"presence\":\"%s\","
		       "\"firmware_version\":\"%s\"}}",
		       WEB_API_PROTOCOL_VERSION, id, provisioned ? "true" : "false", addr,
		       presence, APP_VERSION_STRING);
	if (len > 0 && (size_t)len < sizeof(buf)) {
		web_api_send_line(buf, (size_t)len);
	}
}

static void send_result_volume(int32_t id, uint8_t percent)
{
	char buf[64];
	int len;

	len = snprintf(buf, sizeof(buf), "{\"v\":%d,\"id\":%d,\"ok\":true,\"result\":{\"percent\":%u}}",
		       WEB_API_PROTOCOL_VERSION, id, percent);
	if (len > 0 && (size_t)len < sizeof(buf)) {
		web_api_send_line(buf, (size_t)len);
	}
}

#define WEB_API_LS_MAX_ENTRIES 48
#define WEB_API_LS_NAME_MAX 64

struct ls_ctx {
	char names[WEB_API_LS_MAX_ENTRIES][WEB_API_LS_NAME_MAX];
	size_t count;
};

/* Directories get a trailing "/", same convention as the shell's `ls`. */
static void ls_collect_cb(const char *name, bool is_dir, uint32_t size, void *user_data)
{
	struct ls_ctx *ctx = user_data;

	ARG_UNUSED(size);

	if (ctx->count >= WEB_API_LS_MAX_ENTRIES) {
		return;
	}

	snprintf(ctx->names[ctx->count], WEB_API_LS_NAME_MAX, is_dir ? "%s/" : "%s", name);
	ctx->count++;
}

static void handle_ls(int32_t id)
{
	struct ls_ctx ctx = { .count = 0 };
	char line[CONFIG_WEB_API_LINE_BUF_SIZE];
	char escaped[WEB_API_LS_NAME_MAX * 2];
	size_t pos = 0;
	size_t i;
	int err;

	err = sd_card_list_root(ls_collect_cb, &ctx);
	if (err) {
		send_error(id, "EIO", "Failed to list the SD card's root directory");
		return;
	}

	web_api_append(line, sizeof(line), &pos,
		       "{\"v\":%d,\"id\":%d,\"ok\":true,\"result\":{\"files\":[",
		       WEB_API_PROTOCOL_VERSION, id);

	for (i = 0; i < ctx.count; i++) {
		web_api_append(line, sizeof(line), &pos, "%s\"%s\"", i ? "," : "",
			       web_api_escape(escaped, sizeof(escaped), ctx.names[i]));
	}

	if (pos >= sizeof(line)) {
		send_error(id, "E2BIG", "Directory listing too large for one message");
		return;
	}

	web_api_append(line, sizeof(line), &pos, "]}}");
	if (pos >= sizeof(line)) {
		send_error(id, "E2BIG", "Directory listing too large for one message");
		return;
	}

	web_api_send_line(line, pos);
}

/******************************************************************************/
/****************************** Async events **********************************/
/******************************************************************************/
static void send_event_chat(const char *event, uint16_t addr, const char *text)
{
	char escaped[256];
	char line[320];
	int len;

	if (!web_api.connected) {
		return;
	}

	len = snprintf(line, sizeof(line),
		       "{\"v\":%d,\"event\":\"%s\",\"addr\":\"0x%04x\",\"text\":\"%s\"}",
		       WEB_API_PROTOCOL_VERSION, event, addr,
		       web_api_escape(escaped, sizeof(escaped), text));
	if (len > 0 && (size_t)len < sizeof(line)) {
		web_api_send_line(line, (size_t)len);
	}
}

void web_api_notify_chat_message(uint16_t addr, const char *text)
{
	send_event_chat("chat_message", addr, text);
}

void web_api_notify_chat_private_message(uint16_t addr, const char *text)
{
	send_event_chat("chat_private_message", addr, text);
}

void web_api_notify_chat_message_reply(uint16_t addr, const char *text)
{
	send_event_chat("chat_message_reply", addr, text);
}

void web_api_notify_presence(uint16_t addr, enum bt_mesh_chat_cli_presence presence)
{
	char buf[96];
	int len;

	if (!web_api.connected) {
		return;
	}

	len = snprintf(buf, sizeof(buf), "{\"v\":%d,\"event\":\"presence\",\"addr\":\"0x%04x\",\"presence\":\"%s\"}",
		       WEB_API_PROTOCOL_VERSION, addr, model_handler_presence_to_string(presence));
	if (len > 0 && (size_t)len < sizeof(buf)) {
		web_api_send_line(buf, (size_t)len);
	}
}

/******************************************************************************/
/***************************** Request dispatch ********************************/
/******************************************************************************/
struct web_api_args {
	const char *filename;
	const char *text;
	const char *presence;
	int32_t addr;
	int32_t percent;
};

struct web_api_request {
	int32_t v;
	int32_t id;
	const char *cmd;
	struct web_api_args args;
};

static const struct json_obj_descr args_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct web_api_args, filename, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct web_api_args, text, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct web_api_args, presence, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct web_api_args, addr, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct web_api_args, percent, JSON_TOK_NUMBER),
};

static const struct json_obj_descr request_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct web_api_request, v, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct web_api_request, id, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct web_api_request, cmd, JSON_TOK_STRING),
	JSON_OBJ_DESCR_OBJECT(struct web_api_request, args, args_descr),
};

static void handle_play(int32_t id, const struct web_api_args *args)
{
	int err;

	if (!args->filename) {
		send_error(id, "EINVAL", "Missing \"filename\"");
		return;
	}

	err = audio_player_play_file(args->filename);
	switch (err) {
	case 0:
		send_result_empty(id);
		break;
	case -ENOENT:
		send_error(id, "ENOENT", "No such file in the SD card's root directory");
		break;
	case -EALREADY:
		send_error(id, "EALREADY", "Already playing a file");
		break;
	case -ENODEV:
		send_error(id, "ENODEV", "Audio player not initialized");
		break;
	default:
		send_error(id, "EIO", "Failed to play file");
		break;
	}
}

static void handle_volume(int32_t id, const struct web_api_args *args)
{
	int err;

	if (args->percent >= 0) {
		if (args->percent > 100) {
			send_error(id, "EINVAL", "\"percent\" must be 0-100");
			return;
		}

		err = audio_player_set_volume((uint8_t)args->percent);
		if (err == -ENODEV) {
			send_error(id, "ENODEV", "Audio player not initialized");
			return;
		} else if (err) {
			send_error(id, "EINVAL", "Failed to set volume");
			return;
		}
	}

	send_result_volume(id, audio_player_get_volume());
}

static void handle_status(int32_t id)
{
	bool provisioned;
	uint16_t addr;
	enum bt_mesh_chat_cli_presence presence;

	model_handler_status(&provisioned, &addr, &presence);
	send_result_status(id, provisioned, addr, model_handler_presence_to_string(presence));
}

static void handle_chat_send(int32_t id, const struct web_api_args *args)
{
	int err;

	if (!args->text) {
		send_error(id, "EINVAL", "Missing \"text\"");
		return;
	}

	err = model_handler_chat_send(args->text);
	if (err) {
		send_error(id, "EIO", "Failed to send message");
		return;
	}

	send_result_empty(id);
}

static void handle_chat_send_private(int32_t id, const struct web_api_args *args)
{
	int err;

	if (!args->text || args->addr <= 0) {
		send_error(id, "EINVAL", "Missing \"addr\" or \"text\"");
		return;
	}

	err = model_handler_chat_send_private((uint16_t)args->addr, args->text);
	if (err) {
		send_error(id, "EIO", "Failed to send private message");
		return;
	}

	send_result_empty(id);
}

static void handle_presence_set(int32_t id, const struct web_api_args *args)
{
	enum bt_mesh_chat_cli_presence presence;
	int err;

	if (!args->presence || !model_handler_presence_from_string(args->presence, &presence)) {
		send_error(id, "EINVAL", "Unknown or missing \"presence\"");
		return;
	}

	err = model_handler_presence_set(presence);
	if (err) {
		send_error(id, "EIO", "Failed to update presence");
		return;
	}

	send_result_empty(id);
}

static void handle_presence_get(int32_t id, const struct web_api_args *args)
{
	int err;

	if (args->addr <= 0) {
		send_error(id, "EINVAL", "Missing \"addr\"");
		return;
	}

	err = model_handler_presence_get((uint16_t)args->addr);
	if (err) {
		send_error(id, "EIO", "Failed to request presence");
		return;
	}

	/* The remote node's presence arrives later as an async "presence"
	 * event, not synchronously in this response.
	 */
	send_result_empty(id);
}

static void handle_cue_play(int32_t id, const struct web_api_args *args)
{
	int err;

	if (!args->filename) {
		send_error(id, "EINVAL", "Missing \"filename\"");
		return;
	}

	err = model_handler_cue_play(args->filename);
	if (err) {
		send_error(id, "EIO", "Failed to broadcast play command");
		return;
	}

	send_result_empty(id);
}

static void handle_cue_find(int32_t id, const struct web_api_args *args)
{
	int err;

	if (!args->filename) {
		send_error(id, "EINVAL", "Missing \"filename\"");
		return;
	}

	err = model_handler_cue_find(args->filename);
	if (err) {
		send_error(id, "EIO", "Failed to broadcast find command");
		return;
	}

	send_result_empty(id);
}

static void handle_cue_list(int32_t id)
{
	int err;

	err = model_handler_cue_list();
	if (err) {
		send_error(id, "EIO", "Failed to broadcast list command");
		return;
	}

	send_result_empty(id);
}

static void web_api_handle_line(char *line, size_t len)
{
	struct web_api_request req = {
		.v = 0,
		.id = 0,
		.cmd = NULL,
		.args = {
			.filename = NULL,
			.text = NULL,
			.presence = NULL,
			.addr = 0,
			.percent = -1,
		},
	};
	int64_t mask;

	if (len == 0) {
		return;
	}

	mask = json_obj_parse(line, len, request_descr, ARRAY_SIZE(request_descr), &req);
	if (mask < 0 || !req.cmd) {
		LOG_WRN("Failed to parse request (%lld): %s", mask, line);
		send_error(0, "EINVAL", "Malformed request");
		return;
	}

	if (!strcmp(req.cmd, "ping")) {
		send_result_empty(req.id);
	} else if (!strcmp(req.cmd, "status")) {
		handle_status(req.id);
	} else if (!strcmp(req.cmd, "play")) {
		handle_play(req.id, &req.args);
	} else if (!strcmp(req.cmd, "volume")) {
		handle_volume(req.id, &req.args);
	} else if (!strcmp(req.cmd, "ls")) {
		handle_ls(req.id);
	} else if (!strcmp(req.cmd, "chat_send")) {
		handle_chat_send(req.id, &req.args);
	} else if (!strcmp(req.cmd, "chat_send_private")) {
		handle_chat_send_private(req.id, &req.args);
	} else if (!strcmp(req.cmd, "presence_set")) {
		handle_presence_set(req.id, &req.args);
	} else if (!strcmp(req.cmd, "presence_get")) {
		handle_presence_get(req.id, &req.args);
	} else if (!strcmp(req.cmd, "cue_play")) {
		handle_cue_play(req.id, &req.args);
	} else if (!strcmp(req.cmd, "cue_find")) {
		handle_cue_find(req.id, &req.args);
	} else if (!strcmp(req.cmd, "cue_list")) {
		handle_cue_list(req.id);
	} else {
		send_error(req.id, "EINVAL", "Unknown command");
	}
}

/******************************************************************************/
/******************************** Public API **********************************/
/******************************************************************************/
int web_api_init(void)
{
	if (!device_is_ready(web_api_uart)) {
		LOG_ERR("web_api_cdc_acm_uart not ready");
		return -ENODEV;
	}

	web_api.thread_id = k_thread_create(&web_api.thread, web_api_thread_stack,
					    K_THREAD_STACK_SIZEOF(web_api_thread_stack),
					    web_api_thread, NULL, NULL, NULL,
					    K_PRIO_PREEMPT(WEB_API_THREAD_PRIO), 0,
					    K_NO_WAIT);
	(void)k_thread_name_set(web_api.thread_id, "web_api");

	return 0;
}
