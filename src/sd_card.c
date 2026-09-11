#include "sd_card.h"

#include <string.h>
#include <stdio.h>

#include <ff.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(sd_card, CONFIG_LOG_DEFAULT_LEVEL);

#define SD_CARD_MOUNT_POINT "/SD:"

static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = SD_CARD_MOUNT_POINT,
};

int sd_card_init(void)
{
	int err;

	err = fs_mount(&mp);
	if (err) {
		LOG_ERR("Failed to mount SD card: %d", err);
		return err;
	}

	LOG_INF("SD card mounted at %s", SD_CARD_MOUNT_POINT);
	return 0;
}

bool sd_card_resolve_root_file(const char *filename, char *path_out, size_t path_out_size)
{
	struct fs_dirent entry;
	size_t needed;

	if (!filename || filename[0] == '\0') {
		return false;
	}

	/* Root directory only: reject anything that could reference a
	 * subdirectory or walk out of it.
	 */
	if (strchr(filename, '/') || strchr(filename, '\\') ||
	    strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
		return false;
	}

	needed = strlen(SD_CARD_MOUNT_POINT) + 1 + strlen(filename) + 1;
	if (needed > path_out_size) {
		return false;
	}

	snprintf(path_out, path_out_size, "%s/%s", SD_CARD_MOUNT_POINT, filename);

	if (fs_stat(path_out, &entry) != 0) {
		return false;
	}

	return entry.type == FS_DIR_ENTRY_FILE;
}

static int cmd_ls(const struct shell *shell, size_t argc, char *argv[])
{
	struct fs_dir_t dir;
	int err;

	fs_dir_t_init(&dir);

	err = fs_opendir(&dir, SD_CARD_MOUNT_POINT);
	if (err) {
		shell_error(shell, "Failed to open %s: %d", SD_CARD_MOUNT_POINT, err);
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
