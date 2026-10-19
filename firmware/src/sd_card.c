#include "sd_card.h"

#include <errno.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include <ff.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(sd_card, CONFIG_LOG_DEFAULT_LEVEL);

#define SD_CARD_MOUNT_POINT "/SD:"

/* Matches the README's claim that the VS1053 decodes WAV and MP3
 * identically; anything else is skipped in a playable-file listing.
 */
static bool has_playable_extension(const char *filename)
{
	size_t len = strlen(filename);
	const char *ext = len >= 4 ? filename + len - 4 : filename;

	return len >= 4 && (strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".mp3") == 0);
}

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

int sd_card_list_playable_files(char *out, size_t out_size)
{
	struct fs_dir_t dir;
	int err;
	size_t len = 0;
	int count = 0;

	if (out_size == 0) {
		return -EINVAL;
	}
	out[0] = '\0';

	fs_dir_t_init(&dir);

	err = fs_opendir(&dir, SD_CARD_MOUNT_POINT);
	if (err) {
		return err;
	}

	while (1) {
		struct fs_dirent entry;
		size_t name_len;
		size_t sep_len = count > 0 ? 1 : 0;

		err = fs_readdir(&dir, &entry);
		if (err) {
			break;
		}

		if (entry.name[0] == '\0') {
			break;
		}

		if (entry.type != FS_DIR_ENTRY_FILE || !has_playable_extension(entry.name)) {
			continue;
		}

		name_len = strlen(entry.name);
		if (len + sep_len + name_len + 1 > out_size) {
			/* Out of room; report what fit rather than truncating
			 * a filename mid-way or overflowing the buffer.
			 */
			break;
		}

		if (sep_len) {
			out[len++] = ',';
		}
		memcpy(&out[len], entry.name, name_len);
		len += name_len;
		out[len] = '\0';
		count++;
	}

	fs_closedir(&dir);
	return count;
}

int sd_card_list_root(sd_card_list_root_cb_t cb, void *user_data)
{
	struct fs_dir_t dir;
	int err;

	fs_dir_t_init(&dir);

	err = fs_opendir(&dir, SD_CARD_MOUNT_POINT);
	if (err) {
		return err;
	}

	while (1) {
		struct fs_dirent entry;

		err = fs_readdir(&dir, &entry);
		if (err) {
			break;
		}

		if (entry.name[0] == '\0') {
			err = 0;
			break;
		}

		cb(entry.name, entry.type == FS_DIR_ENTRY_DIR, entry.size, user_data);
	}

	fs_closedir(&dir);
	return err;
}

static void print_ls_entry(const char *name, bool is_dir, uint32_t size, void *user_data)
{
	const struct shell *shell = user_data;

	if (is_dir) {
		shell_print(shell, "%s/", name);
	} else {
		shell_print(shell, "%-32s %u", name, size);
	}
}

static int cmd_ls(const struct shell *shell, size_t argc, char *argv[])
{
	int err;

	err = sd_card_list_root(print_ls_entry, (void *)shell);
	if (err) {
		shell_error(shell, "Failed to list %s: %d", SD_CARD_MOUNT_POINT, err);
	}

	return err;
}

SHELL_CMD_ARG_REGISTER(ls, NULL, "List the SD card's root directory", cmd_ls, 1, 0);
