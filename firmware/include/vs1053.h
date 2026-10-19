#ifndef VS1053_H_
#define VS1053_H_

#include <stddef.h>
#include <stdint.h>

/** @brief Initialize the VS1053 codec: verify DREQ, soft-reset, set clock and volume. */
int vs1053_init(void);

/**
 * @brief Stream a block of file data to the VS1053 decoder over SDI.
 *
 * Blocks until the whole buffer has been sent, waiting on DREQ as needed.
 * The VS1053's onboard firmware auto-detects the audio format (WAV, MP3,
 * OGG, ...) from the stream itself, so @p data should be the raw file
 * bytes with no pre-processing.
 */
int vs1053_play_data(const uint8_t *data, size_t len);

/** @brief Flush the decoder's internal buffer and cancel any decode in progress. */
void vs1053_stop(void);

/** @brief Set output volume. 0x00 is loudest, 0xFE is closest to silent per channel. */
void vs1053_set_volume(uint8_t left_attenuation, uint8_t right_attenuation);

#endif /* VS1053_H_ */
