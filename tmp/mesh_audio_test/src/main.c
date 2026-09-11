/** @file
 *  @brief Bisection step 4 (final): BT Mesh self-provisioning node with
 *  SD card + VS1053 playback triggered by received mesh messages.
 *
 *  A received mesh chat message that matches a filename in the SD
 *  card's root directory plays automatically — see model_handler.c's
 *  play_if_known_file(), which calls audio_player_play_file() below.
 *  The ls/play/volume shell commands from tmp/audio_test are kept too,
 *  for testing independent of mesh (e.g. on a lone, unprovisioned DUT).
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <bluetooth/mesh/models.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include <ff.h>

#include "chat_cli.h"
#include "model_handler.h"
#include "vs1053.h"

LOG_MODULE_REGISTER(chat, CONFIG_LOG_DEFAULT_LEVEL);

/******************************************************************************/
/**************************** SD card + VS1053 *********************************/
/******************************************************************************/
#define SD_MOUNT_POINT "/SD:"
#define PATH_MAX_LEN 128
#define READ_CHUNK 512

/* Matches vs1053.c's own startup default (VS1053_DEFAULT_VOL_ATTEN =
 * 0x20), just expressed on the 0-100 scale used by the shell command
 * below, so the reported "current" volume is right from boot even
 * before the volume command is ever used.
 */
#define VOLUME_DEFAULT_PERCENT 87

static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = SD_MOUNT_POINT,
};
static bool sd_mounted;
static bool vs1053_ready;
static uint8_t volume_percent = VOLUME_DEFAULT_PERCENT;

/* VS1053's SCI_VOL register is an attenuation in 0.5 dB steps: 0x00 is
 * loudest, 0xFE is the quietest non-powerdown value (0xFF cuts the
 * channel's analog output entirely, so it's deliberately never reached
 * here). Map the shell's 0-100 "volume" onto that range.
 */
static uint8_t percent_to_attenuation(uint8_t percent)
{
	return (uint8_t)((100 - percent) * 254 / 100);
}

static int cmd_ls(const struct shell *shell, size_t argc, char *argv[])
{
	struct fs_dir_t dir;
	int err;

	if (!sd_mounted) {
		shell_error(shell, "SD card not mounted");
		return -ENODEV;
	}

	fs_dir_t_init(&dir);

	err = fs_opendir(&dir, SD_MOUNT_POINT);
	if (err) {
		shell_error(shell, "Failed to open %s: %d", SD_MOUNT_POINT, err);
		return err;
	}

	while (1) {
		struct fs_dirent entry;

		err = fs_readdir(&dir, &entry);
		if (err) {
			shell_error(shell, "Read error: %d", err);
			break;
		}

		if (entry.name[0] == '\0') {
			break;
		}

		if (entry.type == FS_DIR_ENTRY_DIR) {
			shell_print(shell, "%s/", entry.name);
		} else {
			shell_print(shell, "%-32s %u", entry.name, entry.size);
		}
	}

	fs_closedir(&dir);
	return 0;
}

SHELL_CMD_ARG_REGISTER(ls, NULL, "List the SD card's root directory", cmd_ls, 1, 0);

/* Root directory only: reject anything that could reference a
 * subdirectory or walk out of it.
 */
static bool resolve_root_file(const char *filename, char *path_out, size_t path_out_size)
{
	struct fs_dirent entry;
	size_t needed;

	if (!filename || filename[0] == '\0') {
		return false;
	}

	if (strchr(filename, '/') || strchr(filename, '\\') || strcmp(filename, ".") == 0 ||
	    strcmp(filename, "..") == 0) {
		return false;
	}

	needed = strlen(SD_MOUNT_POINT) + 1 + strlen(filename) + 1;
	if (needed > path_out_size) {
		return false;
	}

	snprintf(path_out, path_out_size, "%s/%s", SD_MOUNT_POINT, filename);

	if (fs_stat(path_out, &entry) != 0) {
		return false;
	}

	return entry.type == FS_DIR_ENTRY_FILE;
}

/* Shared by the `play` shell command and mesh message reception
 * (model_handler.c's play_if_known_file() calls this by name).
 */
int audio_player_play_file(const char *filename)
{
	char path[PATH_MAX_LEN];
	static uint8_t chunk[READ_CHUNK];
	struct fs_file_t file;
	int err;

	if (!sd_mounted || !vs1053_ready) {
		return -ENODEV;
	}

	if (!resolve_root_file(filename, path, sizeof(path))) {
		return -ENOENT;
	}

	fs_file_t_init(&file);

	err = fs_open(&file, path, FS_O_READ);
	if (err) {
		return err;
	}

	while (1) {
		ssize_t read = fs_read(&file, chunk, sizeof(chunk));

		if (read < 0) {
			err = (int)read;
			break;
		}

		if (read == 0) {
			err = 0;
			break;
		}

		err = vs1053_play_data(chunk, (size_t)read);
		if (err) {
			break;
		}
	}

	vs1053_stop();
	fs_close(&file);

	return err;
}

static int cmd_play(const struct shell *shell, size_t argc, char *argv[])
{
	int err;

	shell_print(shell, "Playing %s", argv[1]);

	err = audio_player_play_file(argv[1]);
	if (err == -ENODEV) {
		shell_error(shell, "SD card or VS1053 not ready");
		return err;
	} else if (err == -ENOENT) {
		shell_error(shell, "No such file in the SD card's root directory: %s", argv[1]);
		return err;
	} else if (err) {
		shell_error(shell, "Playback error: %d", err);
		return err;
	}

	shell_print(shell, "Done");
	return 0;
}

SHELL_CMD_ARG_REGISTER(play, NULL, "Play a file from the SD card's root directory <filename>",
			cmd_play, 2, 0);

/* Volume is a VS1053 hardware register, not a per-file setting: it
 * stays wherever it's set across every subsequent `play` (and every
 * mesh-triggered playback), so it's its own command rather than a
 * `play` argument.
 */
static int cmd_volume(const struct shell *shell, size_t argc, char *argv[])
{
	long percent;
	char *endptr;

	if (argc == 1) {
		shell_print(shell, "Volume: %u%%", volume_percent);
		return 0;
	}

	if (!vs1053_ready) {
		shell_error(shell, "VS1053 not initialized");
		return -ENODEV;
	}

	percent = strtol(argv[1], &endptr, 10);
	if (*endptr != '\0' || percent < 0 || percent > 100) {
		shell_error(shell, "Volume must be 0-100");
		return -EINVAL;
	}

	volume_percent = (uint8_t)percent;
	vs1053_set_volume(percent_to_attenuation(volume_percent),
			   percent_to_attenuation(volume_percent));
	shell_print(shell, "Volume set to %u%%", volume_percent);

	return 0;
}

SHELL_CMD_ARG_REGISTER(volume, NULL, "Get or set output volume, persists across play [0-100]",
			cmd_volume, 1, 1);

static void audio_hw_init(void)
{
	int err;

	err = fs_mount(&mp);
	if (err) {
		LOG_ERR("Failed to mount SD card: %d", err);
		printk("SD card mount failed (err %d)\n", err);
	} else {
		sd_mounted = true;
		printk("SD card mounted at %s\n", SD_MOUNT_POINT);
	}

	err = vs1053_init();
	if (err) {
		LOG_ERR("Failed to initialize VS1053: %d", err);
		printk("VS1053 init failed (err %d)\n", err);
	} else {
		vs1053_ready = true;
		/* Make sure the tracked volume_percent and the chip's actual
		 * register agree from boot, rather than relying on two
		 * separately-maintained defaults staying in sync.
		 */
		vs1053_set_volume(percent_to_attenuation(volume_percent),
				   percent_to_attenuation(volume_percent));
		printk("VS1053 ready (volume %u%%)\n", volume_percent);
	}
}

/******************************************************************************/
/******************************** BT Mesh **************************************/
/******************************************************************************/
/* Pre-shared keys — identical on all nodes. Not for production use. */
static const uint8_t net_key[16] = {
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
};
static const uint8_t dev_key[16] = {
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
};
static const uint8_t app_key[16] = {
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
};

/* Group address for broadcast chat messages */
#define CHAT_GROUP_ADDR 0xC000

static uint8_t dev_uuid[16];
static uint16_t node_addr;

static const struct bt_mesh_prov prov = {
	.uuid = dev_uuid,
};

/* Each node needs a distinct unicast address: two self-provisioned nodes
 * sharing one address silently break the mesh (replay protection is keyed
 * by (address, sequence number), so their traffic starts colliding). Fold
 * the chip's factory-unique hardware ID down to a 15-bit Bluetooth Mesh
 * unicast address instead of relying on a hand-picked, per-board build
 * setting.
 */
static uint16_t derive_node_addr(const uint8_t *id, size_t len)
{
	uint16_t addr = 0;

	for (size_t i = 0; i < len; i++) {
		addr = (uint16_t)((addr * 31) + id[i]);
	}

	/* Bluetooth Mesh unicast addresses are 15-bit and must not be 0. */
	addr &= 0x7FFF;
	if (addr == 0) {
		addr = 1;
	}

	return addr;
}

static void configure(void)
{
	int err;

	printk("Configuring models...\n");

	err = bt_mesh_cfg_cli_app_key_add(0, node_addr, 0, 0, app_key, NULL);
	printk("  App key add: %d\n", err);
	k_sleep(K_MSEC(500));

	err = bt_mesh_cfg_cli_mod_app_bind(0, node_addr, node_addr, 0,
					   BT_MESH_MODEL_ID_HEALTH_SRV, NULL);
	printk("  Health bind: %d\n", err);
	k_sleep(K_MSEC(500));

	err = bt_mesh_cfg_cli_mod_app_bind_vnd(0, node_addr, node_addr, 0,
					       BT_MESH_CHAT_CLI_VENDOR_MODEL_ID,
					       BT_MESH_CHAT_CLI_VENDOR_COMPANY_ID,
					       NULL);
	printk("  Chat bind: %d\n", err);
	k_sleep(K_MSEC(500));

	err = bt_mesh_cfg_cli_mod_sub_add_vnd(0, node_addr, node_addr,
					      CHAT_GROUP_ADDR,
					      BT_MESH_CHAT_CLI_VENDOR_MODEL_ID,
					      BT_MESH_CHAT_CLI_VENDOR_COMPANY_ID,
					      NULL);
	printk("  Chat subscription: %d\n", err);
	k_sleep(K_MSEC(500));

	/* Publication must be set directly; CFG CLI pub_set is fire-and-forget
	 * and the loopback STATUS response is not guaranteed before first send. */
	model_handler_set_pub(CHAT_GROUP_ADDR, 0, BT_MESH_TTL_DEFAULT);

	printk("Self-configuration complete\n");
}

static void bt_ready(int err)
{
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");

	audio_hw_init();

	err = bt_mesh_init(&prov, model_handler_init());
	if (err) {
		printk("Initializing mesh failed (err %d)\n", err);
		return;
	}

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	if (bt_mesh_is_provisioned() && model_handler_addr() != node_addr) {
		/* Stale provisioning from a different addressing scheme (e.g.
		 * an old fixed NODE_ADDR, or a previous test image) - reset
		 * once so this device re-provisions at its current
		 * node_addr. Only do this on an actual mismatch: resetting
		 * every boot would restart this node's sequence number at 0
		 * each time, which other nodes' replay protection would
		 * eventually start treating as a replay of already-seen
		 * traffic and silently drop.
		 */
		printk("Stale provisioning at 0x%04x, resetting to rejoin as 0x%04x\n",
		       model_handler_addr(), node_addr);
		bt_mesh_reset();
	}

	err = bt_mesh_provision(net_key, 0, 0, 0, node_addr, dev_key);
	if (err == -EALREADY) {
		printk("Using stored provisioning data (0x%04x)\n", node_addr);
	} else if (err) {
		printk("Provisioning failed (err %d)\n", err);
		return;
	} else {
		printk("Provisioned as node 0x%04x\n", node_addr);
	}

	k_sleep(K_MSEC(1000));
	configure();

	printk("Mesh initialized\n");
}

int main(void)
{
	int err;

	printk("Initializing...\n");

	ssize_t id_len = hwinfo_get_device_id(dev_uuid, sizeof(dev_uuid));

	if (id_len < 0) {
		printk("hwinfo_get_device_id failed (err %d)\n", (int)id_len);
		id_len = 0;
	}
	node_addr = derive_node_addr(dev_uuid, (size_t)id_len);
	printk("Derived mesh node address: 0x%04x\n", node_addr);

	err = bt_enable(bt_ready);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
	}

	return 0;
}
