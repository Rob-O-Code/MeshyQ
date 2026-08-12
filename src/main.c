/** @file
 *  @brief MeshyQ self-provisioning mesh chat node
 */
#include <zephyr/bluetooth/bluetooth.h>
#include <bluetooth/mesh/models.h>
#include <dk_buttons_and_leds.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include "chat_cli.h"
#include "model_handler.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(chat, CONFIG_LOG_DEFAULT_LEVEL);

/* DK library headers define BTN1..BTN4. On nRF5340 Audio DK, SW5 maps to bit 4. */
#ifndef DK_BTN5_MSK
#define DK_BTN5_MSK BIT(4)
#endif

/* Pre-shared keys — identical on all nodes. Not for production use. */
static const uint8_t net_key[16] = {
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
};
static const uint8_t dev_key[16] = {
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
};
static const uint8_t app_key[16] = {
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
};

#define NODE_ADDR CONFIG_NODE_ADDR

/* Group address for broadcast chat messages */
#define CHAT_GROUP_ADDR 0xC000

static uint8_t dev_uuid[16];

struct color_command {
	uint32_t button_mask;
	const char *name;
	const char *rgb_bits;
};

static const struct color_command color_commands[] = {
	{ DK_BTN1_MSK, "OFF", "000" },
	{ DK_BTN2_MSK, "RED", "100" },
	{ DK_BTN3_MSK, "GREEN", "010" },
	{ DK_BTN4_MSK, "BLUE", "001" },
	{ DK_BTN5_MSK, "MAGENTA", "101" },
};

static void button_handler(uint32_t button_state, uint32_t has_changed)
{
	const uint32_t pressed = button_state & has_changed;
	const uint32_t user_buttons = DK_BTN1_MSK | DK_BTN2_MSK |
		DK_BTN3_MSK | DK_BTN4_MSK
		| DK_BTN5_MSK
		;
	const uint32_t active_buttons = button_state & user_buttons;
	const struct color_command *color;
	int err;
	size_t i;

	if (!pressed) {
		return;
	}

	color = NULL;
	for (i = 0; i < ARRAY_SIZE(color_commands); i++) {
		if (active_buttons == color_commands[i].button_mask) {
			color = &color_commands[i];
			break;
		}
	}

	if (!color) {
		return;
	}

	err = model_handler_broadcast_text(color->rgb_bits);
	if (err) {
		printk("Broadcast color %s failed (err %d)\n", color->name, err);
	} else {
		printk("Broadcast color %s (%s)\n", color->name, color->rgb_bits);
	}
}

static const struct bt_mesh_prov prov = {
	.uuid = dev_uuid,
};

static void configure(void)
{
	int err;

	printk("Configuring models...\n");

	err = bt_mesh_cfg_cli_app_key_add(0, NODE_ADDR, 0, 0, app_key, NULL);
	printk("  App key add: %d\n", err);
	k_sleep(K_MSEC(500));

	err = bt_mesh_cfg_cli_mod_app_bind(0, NODE_ADDR, NODE_ADDR, 0,
					   BT_MESH_MODEL_ID_HEALTH_SRV, NULL);
	printk("  Health bind: %d\n", err);
	k_sleep(K_MSEC(500));

	err = bt_mesh_cfg_cli_mod_app_bind_vnd(0, NODE_ADDR, NODE_ADDR, 0,
					       BT_MESH_CHAT_CLI_VENDOR_MODEL_ID,
					       BT_MESH_CHAT_CLI_VENDOR_COMPANY_ID,
					       NULL);
	printk("  Chat bind: %d\n", err);
	k_sleep(K_MSEC(500));

	err = bt_mesh_cfg_cli_mod_sub_add_vnd(0, NODE_ADDR, NODE_ADDR,
					      CHAT_GROUP_ADDR,
					      BT_MESH_CHAT_CLI_VENDOR_MODEL_ID,
					      BT_MESH_CHAT_CLI_VENDOR_COMPANY_ID,
					      NULL);
	printk("  Chat subscription: %d\n", err);
	k_sleep(K_MSEC(500));

	/* Publication must be set directly; CFG CLI pub_set is fire-and-forget
	 * and the loopback STATUS response is not guaranteed before first send. */
	model_handler_set_pub(CHAT_GROUP_ADDR, 0, BT_MESH_TTL_DEFAULT);

	printk("Self-configuration complete\n");
}

static void bt_ready(int err)
{
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");

	err = dk_leds_init();
	if (err) {
		printk("Initializing LEDs failed (err %d)\n", err);
		return;
	}

	err = dk_buttons_init(button_handler);
	if (err) {
		printk("Initializing buttons failed (err %d)\n", err);
		return;
	}

	err = bt_mesh_init(&prov, model_handler_init());
	if (err) {
		printk("Initializing mesh failed (err %d)\n", err);
		return;
	}

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	err = bt_mesh_provision(net_key, 0, 0, 0, NODE_ADDR, dev_key);
	if (err == -EALREADY) {
		printk("Using stored provisioning data\n");
	} else if (err) {
		printk("Provisioning failed (err %d)\n", err);
		return;
	} else {
		printk("Provisioned as node 0x%04x\n", NODE_ADDR);
	}

	k_sleep(K_MSEC(1000));
	configure();

	printk("Mesh initialized\n");
}

int main(void)
{
	int err;

	printk("Initializing...\n");

	hwinfo_get_device_id(dev_uuid, sizeof(dev_uuid));

	err = bt_enable(bt_ready);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
	}

	return 0;
}
