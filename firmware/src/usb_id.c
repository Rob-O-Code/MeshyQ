/** @file
 *  @brief Custom USB device identity for the MeshyQ fleet.
 *
 *  The board's default USB backend (subsys/usb/device_next/app/cdc_acm_serial.c,
 *  enabled by CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT) reports a
 *  compile-time-fixed manufacturer/product string ("Zephyr Project" /
 *  "CDC ACM serial backend"), so every node in the fleet looks identical on
 *  the host. Kconfig strings can't embed a value read from HWINFO at
 *  runtime, so this file replaces that default backend with an equivalent
 *  single CDC-ACM-instance USB device whose product string folds in part of
 *  this chip's hardware UID (e.g. "MeshyQ A1B2C3D4"), making each node
 *  distinguishable. CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT is disabled in
 *  prj.conf so the two don't both try to claim the same UDC device.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/usb/usbd.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(usb_id, CONFIG_USBD_LOG_LEVEL);

#define MESHYQ_USB_VID 0x2FE3
#define MESHYQ_USB_PID 0x0004
#define MESHYQ_USB_MAX_POWER 125 /* 250 mA, in 2 mA units */

#define MESHYQ_PRODUCT_TEMPLATE "MeshyQ XXXXXXXX"

static uint8_t meshyq_product_ascii[sizeof(MESHYQ_PRODUCT_TEMPLATE) - 1];

USBD_DEVICE_DEFINE(meshyq_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   MESHYQ_USB_VID, MESHYQ_USB_PID);

USBD_DESC_LANG_DEFINE(meshyq_usbd_lang);
USBD_DESC_MANUFACTURER_DEFINE(meshyq_usbd_mfr, "MeshyQ");
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(meshyq_usbd_sn)));

static struct usbd_desc_node meshyq_usbd_product = {
	.str = {
		.utype = USBD_DUT_STRING_PRODUCT,
		.ascii7 = true,
	},
	.ptr = meshyq_product_ascii,
	.bLength = USB_STRING_DESCRIPTOR_LENGTH(MESHYQ_PRODUCT_TEMPLATE),
	.bDescriptorType = USB_DESC_STRING,
};

USBD_DESC_CONFIG_DEFINE(meshyq_fs_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(meshyq_fs_config, 0, MESHYQ_USB_MAX_POWER, &meshyq_fs_cfg_desc);

static void meshyq_build_product_string(void)
{
	uint8_t uid[4] = {0};
	char text[sizeof(MESHYQ_PRODUCT_TEMPLATE)];

	if (hwinfo_get_device_id(uid, sizeof(uid)) < 0) {
		LOG_WRN("hwinfo_get_device_id failed, USB product string will use a zeroed ID");
	}

	snprintf(text, sizeof(text), "MeshyQ %02X%02X%02X%02X",
		 uid[0], uid[1], uid[2], uid[3]);
	memcpy(meshyq_product_ascii, text, sizeof(meshyq_product_ascii));
}

static int meshyq_usb_init(void)
{
	int err;

	meshyq_build_product_string();

	err = usbd_add_descriptor(&meshyq_usbd, &meshyq_usbd_lang);
	if (err) {
		LOG_ERR("Failed to add language descriptor (%d)", err);
		return err;
	}

	err = usbd_add_descriptor(&meshyq_usbd, &meshyq_usbd_mfr);
	if (err) {
		LOG_ERR("Failed to add manufacturer descriptor (%d)", err);
		return err;
	}

	err = usbd_add_descriptor(&meshyq_usbd, &meshyq_usbd_product);
	if (err) {
		LOG_ERR("Failed to add product descriptor (%d)", err);
		return err;
	}

	if (IS_ENABLED(CONFIG_HWINFO)) {
		err = usbd_add_descriptor(&meshyq_usbd, &meshyq_usbd_sn);
		if (err) {
			LOG_ERR("Failed to add serial number descriptor (%d)", err);
			return err;
		}
	}

	err = usbd_add_configuration(&meshyq_usbd, USBD_SPEED_FS, &meshyq_fs_config);
	if (err) {
		LOG_ERR("Failed to add configuration (%d)", err);
		return err;
	}

	/* Matches the devicetree-registered CDC ACM UART backing zephyr,console. */
	err = usbd_register_class(&meshyq_usbd, "cdc_acm_0", USBD_SPEED_FS, 1);
	if (err) {
		LOG_ERR("Failed to register cdc_acm_0 (%d)", err);
		return err;
	}

	err = usbd_device_set_code_triple(&meshyq_usbd, USBD_SPEED_FS,
					  USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	if (err) {
		LOG_ERR("Failed to set code triple (%d)", err);
		return err;
	}

	err = usbd_init(&meshyq_usbd);
	if (err) {
		LOG_ERR("Failed to init USB device support (%d)", err);
		return err;
	}

	err = usbd_enable(&meshyq_usbd);
	if (err) {
		LOG_ERR("Failed to enable USB device support (%d)", err);
		return err;
	}

	return 0;
}

SYS_INIT(meshyq_usb_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
