#ifndef SD_CARD_H_
#define SD_CARD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/**
 * @brief Build a comma-separated list of playable (.wav/.mp3) files in the
 * SD card's root directory.
 *
 * If the buffer is too small to hold every playable file, the list is cut
 * off at the last file that fit rather than a partial filename.
 *
 * @param[out] out      Buffer to receive the comma-separated, null-terminated
 * list. Left as an empty string if there are no playable files.
 * @param[in]  out_size Size of @p out. Must be at least 1.
 *
 * @retval >=0 Number of files included in the list.
 * @retval -EINVAL @p out_size is 0.
 * @retval other Negative errno from opening the root directory.
 */
int sd_card_list_playable_files(char *out, size_t out_size);

/** @brief Called once per entry by sd_card_list_root(). */
typedef void (*sd_card_list_root_cb_t)(const char *name, bool is_dir, uint32_t size,
					void *user_data);

/**
 * @brief Walk the SD card's root directory, calling @p cb for each entry.
 *
 * @param cb        Called once per directory entry.
 * @param user_data Passed through to @p cb unchanged.
 *
 * @retval 0      Directory listed successfully.
 * @retval other  Negative errno from opening/reading the root directory.
 */
int sd_card_list_root(sd_card_list_root_cb_t cb, void *user_data);

#endif /* SD_CARD_H_ */
