#ifndef SD_CARD_H_
#define SD_CARD_H_

#include <stdbool.h>
#include <stddef.h>

/** @brief Mount the FAT filesystem on the SD card. */
int sd_card_init(void);

/**
 * @brief Resolve a bare filename to a path in the SD card's root directory,
 * and check that it exists there as a regular file.
 *
 * Only the root directory is searched: @p filename must not contain a path
 * separator, so mesh messages can't be used to reach into subdirectories.
 *
 * @param filename    Bare filename, e.g. "example.wav".
 * @param path_out    Buffer to receive the resolved path.
 * @param path_out_size Size of @p path_out.
 *
 * @retval true  The file exists in the SD card's root directory.
 * @retval false The name is invalid, or no such file exists.
 */
bool sd_card_resolve_root_file(const char *filename, char *path_out, size_t path_out_size);

#endif /* SD_CARD_H_ */
