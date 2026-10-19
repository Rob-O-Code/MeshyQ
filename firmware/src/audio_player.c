#include "audio_player.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "sd_card.h"
#include "vs1053.h"

LOG_MODULE_REGISTER(audio_player, CONFIG_LOG_DEFAULT_LEVEL);

#define AUDIO_PLAYER_PATH_MAX 128
#define AUDIO_PLAYER_READ_CHUNK 512

/* Matches vs1053.c's own startup default (VS1053_DEFAULT_VOL_ATTEN =
 * 0x20), just expressed on the 0-100 scale used by the shell command
 * below, so the reported "current" volume is right from boot even
 * before the volume command is ever used.
 */
#define VOLUME_DEFAULT_PERCENT 87
/* Generous stack: this thread also does the SD card mount and FAT
 * filesystem bring-up, which use more stack than typical playback code
 * (FatFs keeps per-call work buffers on the stack). Sizing this thread's
 * stack independently of the main thread keeps a slow or failing SD card
 * from ever touching main()'s stack or blocking bt_ready()/the shell.
 */
#define AUDIO_THREAD_STACK_SIZE 4096
#define AUDIO_THREAD_PRIO 7

K_THREAD_STACK_DEFINE(audio_thread_stack, AUDIO_THREAD_STACK_SIZE);

static struct {
	struct k_sem play_request;
	struct k_mutex lock;
	struct k_thread thread;
	k_tid_t thread_id;
	bool ready;
	bool busy;
	char pending_path[AUDIO_PLAYER_PATH_MAX];
} player;

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

static void play_file(const char *path)
{
	static uint8_t chunk[AUDIO_PLAYER_READ_CHUNK];
	struct fs_file_t file;
	int err;

	fs_file_t_init(&file);

	err = fs_open(&file, path, FS_O_READ);
	if (err) {
		LOG_WRN("Failed to open %s: %d", path, err);
		return;
	}

	while (1) {
		ssize_t read = fs_read(&file, chunk, sizeof(chunk));

		if (read < 0) {
			LOG_WRN("Read error on %s: %d", path, (int)read);
			break;
		}

		if (read == 0) {
			break;
		}

		err = vs1053_play_data(chunk, (size_t)read);
		if (err) {
			LOG_WRN("Playback error on %s: %d", path, err);
			break;
		}
	}

	vs1053_stop();
	fs_close(&file);
}

static void audio_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	/* SD mount and VS1053 bring-up happen here, on this thread's own
	 * stack, instead of inline in audio_player_init(). That keeps a
	 * slow or misbehaving SD card / codec from blocking bt_ready() (and
	 * therefore Bluetooth/mesh bring-up) or starving the shell thread.
	 */
	if (sd_card_init() || vs1053_init()) {
		LOG_ERR("Audio hardware bring-up failed; playback disabled");
		return;
	}

	/* Make sure the tracked volume_percent and the chip's actual
	 * register agree from boot, rather than relying on two separately
	 * maintained defaults staying in sync.
	 */
	vs1053_set_volume(percent_to_attenuation(volume_percent),
			   percent_to_attenuation(volume_percent));

	k_mutex_lock(&player.lock, K_FOREVER);
	player.ready = true;
	k_mutex_unlock(&player.lock);

	while (1) {
		char path[AUDIO_PLAYER_PATH_MAX];

		k_sem_take(&player.play_request, K_FOREVER);

		k_mutex_lock(&player.lock, K_FOREVER);
		strcpy(path, player.pending_path);
		k_mutex_unlock(&player.lock);

		play_file(path);

		k_mutex_lock(&player.lock, K_FOREVER);
		player.busy = false;
		k_mutex_unlock(&player.lock);
	}
}

int audio_player_init(void)
{
	k_sem_init(&player.play_request, 0, 1);
	k_mutex_init(&player.lock);

	player.thread_id = k_thread_create(&player.thread, audio_thread_stack,
					   K_THREAD_STACK_SIZEOF(audio_thread_stack),
					   audio_thread, NULL, NULL, NULL,
					   K_PRIO_PREEMPT(AUDIO_THREAD_PRIO), 0,
					   K_NO_WAIT);
	(void)k_thread_name_set(player.thread_id, "audio_player");

	return 0;
}

int audio_player_play_file(const char *filename)
{
	char path[AUDIO_PLAYER_PATH_MAX];

	k_mutex_lock(&player.lock, K_FOREVER);
	if (!player.ready) {
		k_mutex_unlock(&player.lock);
		return -ENODEV;
	}
	k_mutex_unlock(&player.lock);

	if (!sd_card_resolve_root_file(filename, path, sizeof(path))) {
		return -ENOENT;
	}

	k_mutex_lock(&player.lock, K_FOREVER);
	if (player.busy) {
		k_mutex_unlock(&player.lock);
		return -EALREADY;
	}

	player.busy = true;
	strcpy(player.pending_path, path);
	k_mutex_unlock(&player.lock);

	k_sem_give(&player.play_request);
	return 0;
}

/* Local-only trigger: plays straight from the shell, independent of mesh
 * state, so a single DUT with nothing else on the network can still be
 * tested.
 */
static int cmd_play(const struct shell *shell, size_t argc, char *argv[])
{
	int err;

	err = audio_player_play_file(argv[1]);
	switch (err) {
	case 0:
		shell_print(shell, "Playing %s", argv[1]);
		break;
	case -ENOENT:
		shell_error(shell, "No such file in the SD card's root directory: %s", argv[1]);
		break;
	case -EALREADY:
		shell_error(shell, "Already playing a file");
		break;
	case -ENODEV:
		shell_error(shell, "Audio player not initialized");
		break;
	default:
		shell_error(shell, "Failed to play %s: %d", argv[1], err);
		break;
	}

	return 0;
}

SHELL_CMD_ARG_REGISTER(play, NULL,
			"Play a file from the SD card's root directory <filename>", cmd_play, 2,
			0);

/* Volume is a VS1053 hardware register, not a per-file setting: it stays
 * wherever it's set across every subsequent play (and every mesh-triggered
 * playback), so it's its own command rather than a `play` argument.
 */
static int cmd_volume(const struct shell *shell, size_t argc, char *argv[])
{
	long percent;
	char *endptr;
	bool ready;

	if (argc == 1) {
		shell_print(shell, "Volume: %u%%", volume_percent);
		return 0;
	}

	k_mutex_lock(&player.lock, K_FOREVER);
	ready = player.ready;
	k_mutex_unlock(&player.lock);

	if (!ready) {
		shell_error(shell, "Audio player not initialized");
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
