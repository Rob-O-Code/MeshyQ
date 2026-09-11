#ifndef AUDIO_PLAYER_H_
#define AUDIO_PLAYER_H_

/**
 * @brief Play a file from the SD card's root directory, if it exists.
 *
 * Implemented in src/main.c for this test (synchronously, on whatever
 * thread calls it), matching the signature model_handler.c expects.
 *
 * @retval 0        Playback finished.
 * @retval -ENOENT  No such file in the SD card's root directory.
 * @retval -ENODEV  SD card or VS1053 not ready.
 */
int audio_player_play_file(const char *filename);

#endif
