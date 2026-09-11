#ifndef AUDIO_PLAYER_H_
#define AUDIO_PLAYER_H_

/** @brief Mount the SD card and bring up the VS1053 codec. */
int audio_player_init(void);

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
