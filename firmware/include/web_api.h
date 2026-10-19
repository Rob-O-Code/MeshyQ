#ifndef WEB_API_H_
#define WEB_API_H_

#include <stdint.h>

#include <zephyr/usb/usbd.h>

#include "chat_cli.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the structured JSON web API on its own USB CDC-ACM interface
 * (web_api_cdc_acm_uart, see boards, *.overlay), independent of the interactive
 * shell/console on the first CDC-ACM interface.
 */
int web_api_init(void);

/**
 * @brief USB device message hook, called by usb_id.c for every USB device
 * event. Used to detect DTR assertion on web_api_cdc_acm_uart specifically,
 * so the web API only starts processing once a client has actually opened
 * the port. Ignores messages for any other device.
 */
void web_api_usbd_msg_handler(struct usbd_context *ctx, const struct usbd_msg *msg);

/**
 * @brief Notify the web API client of an incoming non-private chat/cue
 * message, mirroring what handle_chat_message() already prints to the
 * shell. No-op if the web API isn't connected.
 */
void web_api_notify_chat_message(uint16_t addr, const char *text);

/** @brief Like web_api_notify_chat_message(), for private messages. */
void web_api_notify_chat_private_message(uint16_t addr, const char *text);

/** @brief Like web_api_notify_chat_message(), for replies (e.g. "found <file>"). */
void web_api_notify_chat_message_reply(uint16_t addr, const char *text);

/** @brief Notify the web API client of a presence change/report. */
void web_api_notify_presence(uint16_t addr, enum bt_mesh_chat_cli_presence presence);

#ifdef __cplusplus
}
#endif

#endif /* WEB_API_H_ */
