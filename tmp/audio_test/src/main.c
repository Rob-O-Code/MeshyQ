/*
 * Bisection step 3: mount the SD card, bring up the VS1053, and play a
 * file from the SD card's root directory by name from the shell. Still
 * no Bluetooth/mesh — isolating the VS1053 SPI wiring/driver from the
 * rest of the main MeshyQ app.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include <ff.h>

#include "vs1053.h"

LOG_MODULE_REGISTER(audio_test, CONFIG_LOG_DEFAULT_LEVEL);

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

static int cmd_play(const struct shell *shell, size_t argc, char *argv[])
{
	char path[PATH_MAX_LEN];
	static uint8_t chunk[READ_CHUNK];
	struct fs_file_t file;
	int err;

	if (!sd_mounted) {
		shell_error(shell, "SD card not mounted");
		return -ENODEV;
	}

	if (!vs1053_ready) {
		shell_error(shell, "VS1053 not initialized");
		return -ENODEV;
	}

	if (!resolve_root_file(argv[1], path, sizeof(path))) {
		shell_error(shell, "No such file in the SD card's root directory: %s", argv[1]);
		return -ENOENT;
	}

	fs_file_t_init(&file);

	err = fs_open(&file, path, FS_O_READ);
	if (err) {
		shell_error(shell, "Failed to open %s: %d", path, err);
		return err;
	}

	shell_print(shell, "Playing %s", argv[1]);

	while (1) {
		ssize_t read = fs_read(&file, chunk, sizeof(chunk));

		if (read < 0) {
			shell_error(shell, "Read error: %d", (int)read);
			break;
		}

		if (read == 0) {
			break;
		}

		err = vs1053_play_data(chunk, (size_t)read);
		if (err) {
			shell_error(shell, "Playback error: %d", err);
			break;
		}
	}

	vs1053_stop();
	fs_close(&file);

	shell_print(shell, "Done");
	return 0;
}

SHELL_CMD_ARG_REGISTER(play, NULL, "Play a file from the SD card's root directory <filename>",
			cmd_play, 2, 0);

/* Volume is a VS1053 hardware register, not a per-file setting: it
 * stays wherever it's set across every subsequent `play`, so it's its
 * own command rather than a `play` argument.
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

int main(void)
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

	return 0;
}
