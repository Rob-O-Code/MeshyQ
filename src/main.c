/** @file
 *  @brief MeshyQ self-provisioning mesh chat node
 */
#include <zephyr/bluetooth/bluetooth.h>
#include <bluetooth/mesh/models.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include "chat_cli.h"
#include "audio_player.h"
#include "model_handler.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(chat, CONFIG_LOG_DEFAULT_LEVEL);

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

/* Group address for broadcast chat messages */
#define CHAT_GROUP_ADDR 0xC000

static uint8_t dev_uuid[16];
static uint16_t node_addr;

static const struct bt_mesh_prov prov = {
	.uuid = dev_uuid,
};

/* Each node needs a distinct unicast address: two self-provisioned nodes
 * sharing one address silently break the mesh (replay protection is keyed
 * by (address, sequence number), so their traffic starts colliding). Fold
 * the chip's factory-unique hardware ID down to a 15-bit Bluetooth Mesh
 * unicast address instead of relying on a hand-picked, per-board build
 * setting.
 */
static uint16_t derive_node_addr(const uint8_t *id, size_t len)
{
	uint16_t addr = 0;

	for (size_t i = 0; i < len; i++) {
		addr = (uint16_t)((addr * 31) + id[i]);
	}

	/* Bluetooth Mesh unicast addresses are 15-bit and must not be 0. */
	addr &= 0x7FFF;
	if (addr == 0) {
		addr = 1;
	}

	return addr;
}

static void configure(void)
{
	int err;

	printk("Configuring models...\n");

	err = bt_mesh_cfg_cli_app_key_add(0, node_addr, 0, 0, app_key, NULL);
	printk("  App key add: %d\n", err);
	k_sleep(K_MSEC(500));

	err = bt_mesh_cfg_cli_mod_app_bind(0, node_addr, node_addr, 0,
					   BT_MESH_MODEL_ID_HEALTH_SRV, NULL);
	printk("  Health bind: %d\n", err);
	k_sleep(K_MSEC(500));

	err = bt_mesh_cfg_cli_mod_app_bind_vnd(0, node_addr, node_addr, 0,
					       BT_MESH_CHAT_CLI_VENDOR_MODEL_ID,
					       BT_MESH_CHAT_CLI_VENDOR_COMPANY_ID,
					       NULL);
	printk("  Chat bind: %d\n", err);
	k_sleep(K_MSEC(500));

	err = bt_mesh_cfg_cli_mod_sub_add_vnd(0, node_addr, node_addr,
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

	err = audio_player_init();
	if (err) {
		printk("Initializing audio playback failed (err %d)\n", err);
	}

	err = bt_mesh_init(&prov, model_handler_init());
	if (err) {
		printk("Initializing mesh failed (err %d)\n", err);
		return;
	}

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	if (bt_mesh_is_provisioned() && model_handler_addr() != node_addr) {
		/* Stale provisioning from a different addressing scheme (e.g.
		 * an old fixed NODE_ADDR, or a previous test image) - reset
		 * once so this device re-provisions at its current
		 * node_addr. Only do this on an actual mismatch: resetting
		 * every boot would restart this node's sequence number at 0
		 * each time, which other nodes' replay protection would
		 * eventually start treating as a replay of already-seen
		 * traffic and silently drop.
		 */
		printk("Stale provisioning at 0x%04x, resetting to rejoin as 0x%04x\n",
		       model_handler_addr(), node_addr);
		bt_mesh_reset();
	}

	err = bt_mesh_provision(net_key, 0, 0, 0, node_addr, dev_key);
	if (err == -EALREADY) {
		printk("Using stored provisioning data (0x%04x)\n", node_addr);
	} else if (err) {
		printk("Provisioning failed (err %d)\n", err);
		return;
	} else {
		printk("Provisioned as node 0x%04x\n", node_addr);
	}

	k_sleep(K_MSEC(1000));
	configure();

	printk("Mesh initialized\n");
}

int main(void)
{
	int err;

	printk("Initializing...\n");

	ssize_t id_len = hwinfo_get_device_id(dev_uuid, sizeof(dev_uuid));

	if (id_len < 0) {
		printk("hwinfo_get_device_id failed (err %d)\n", (int)id_len);
		id_len = 0;
	}
	node_addr = derive_node_addr(dev_uuid, (size_t)id_len);
	printk("Derived mesh node address: 0x%04x\n", node_addr);

	err = bt_enable(bt_ready);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
	}

	return 0;
}
