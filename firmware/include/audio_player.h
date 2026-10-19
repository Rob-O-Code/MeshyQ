#ifndef AUDIO_PLAYER_H_
#define AUDIO_PLAYER_H_

#include <stdint.h>

/** @brief Mount the SD card and bring up the VS1053 codec. */
int audio_player_init(void);

/** @brief Get the current output volume, 0-100. */
uint8_t audio_player_get_volume(void);

/**
 * @brief Set the output volume.
 *
 * @retval 0        Volume set.
 * @retval -EINVAL  percent is not in 0-100.
 * @retval -ENODEV  Audio player has not been initialized.
 */
int audio_player_set_volume(uint8_t percent);

/**
 * @brief Play a file from the SD card's root directory, if it exists.
 *
 * @retval 0        Playback started.
 * @retval -ENOENT  No such file in the SD card's root directory.
 * @retval -EALREADY A file is already playing.
 * @retval -ENODEV  Audio player has not been initialized.
 */
int audio_player_play_file(const char *filename);

#endif
