/*
 * Bisection step 2: mount the SD card and list its root directory from
 * the shell. Still no Bluetooth, no VS1053 — isolating the SPI/SD/FAT
 * filesystem path on its own.
 */
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include <ff.h>

LOG_MODULE_REGISTER(sd_test, CONFIG_LOG_DEFAULT_LEVEL);

#define SD_MOUNT_POINT "/SD:"

static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = SD_MOUNT_POINT,
};
static bool sd_mounted;

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

int main(void)
{
	int err;

	err = fs_mount(&mp);
	if (err) {
		LOG_ERR("Failed to mount SD card: %d", err);
		printk("SD card mount failed (err %d)\n", err);
		return 0;
	}

	sd_mounted = true;
	printk("SD card mounted at %s\n", SD_MOUNT_POINT);
	return 0;
}
