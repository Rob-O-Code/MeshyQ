#define DT_DRV_COMPAT adafruit_vs1053

#include "vs1053.h"

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(vs1053, CONFIG_LOG_DEFAULT_LEVEL);

/* SCI (command) registers. */
#define VS1053_REG_MODE   0x00
#define VS1053_REG_STATUS 0x01
#define VS1053_REG_CLOCKF 0x03
#define VS1053_REG_VOL    0x0B

/* SCI_MODE bits. */
#define VS1053_SM_RESET   0x0004
#define VS1053_SM_CANCEL  0x0008
#define VS1053_SM_SDINEW  0x0800

/* Startup clock multiplier: 3.0x XTALI, matches Adafruit's reference
 * firmware for this wing's 12.288 MHz crystal.
 */
#define VS1053_CLOCKF_VAL 0x6000

#define VS1053_DEFAULT_VOL_ATTEN 0x20

#define VS1053_DREQ_TIMEOUT_MS   500
#define VS1053_SDI_BLOCK_SIZE    32
#define VS1053_DATA_SPI_HZ       4000000

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
	     "Exactly one enabled adafruit,vs1053 node is required");

static const struct spi_dt_spec spi = SPI_DT_SPEC_INST_GET(
	0, SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8) | SPI_LINES_SINGLE, 0);
static const struct gpio_dt_spec dreq_gpio = GPIO_DT_SPEC_INST_GET(0, dreq_gpios);
static const struct gpio_dt_spec xdcs_gpio = GPIO_DT_SPEC_INST_GET(0, xdcs_gpios);

static int wait_for_dreq(void)
{
	int64_t deadline = k_uptime_get() + VS1053_DREQ_TIMEOUT_MS;

	while (gpio_pin_get_dt(&dreq_gpio) == 0) {
		if (k_uptime_get() > deadline) {
			LOG_ERR("Timed out waiting for DREQ");
			return -ETIMEDOUT;
		}
		/* Sleep rather than spin: DREQ can stay low for a while
		 * during normal playback, and this must not monopolize the
		 * CPU or starve other threads while it waits.
		 */
		k_msleep(1);
	}

	return 0;
}

static int sci_write(uint8_t addr, uint16_t value)
{
	uint8_t tx_buf[4] = { 0x02, addr, value >> 8, value & 0xFF };
	struct spi_buf buf = { .buf = tx_buf, .len = sizeof(tx_buf) };
	struct spi_buf_set tx = { .buffers = &buf, .count = 1 };
	int err;

	err = wait_for_dreq();
	if (err) {
		return err;
	}

	err = spi_write_dt(&spi, &tx);
	if (err) {
		LOG_ERR("SCI write failed: %d", err);
		return err;
	}

	return 0;
}

static int sci_read(uint8_t addr, uint16_t *value)
{
	uint8_t tx_buf[4] = { 0x03, addr, 0x00, 0x00 };
	uint8_t rx_buf[4] = { 0 };
	struct spi_buf tx_spi_buf = { .buf = tx_buf, .len = sizeof(tx_buf) };
	struct spi_buf rx_spi_buf = { .buf = rx_buf, .len = sizeof(rx_buf) };
	struct spi_buf_set tx = { .buffers = &tx_spi_buf, .count = 1 };
	struct spi_buf_set rx = { .buffers = &rx_spi_buf, .count = 1 };
	int err;

	err = wait_for_dreq();
	if (err) {
		return err;
	}

	err = spi_transceive_dt(&spi, &tx, &rx);
	if (err) {
		LOG_ERR("SCI read failed: %d", err);
		return err;
	}

	*value = ((uint16_t)rx_buf[2] << 8) | rx_buf[3];
	return 0;
}

void vs1053_set_volume(uint8_t left_attenuation, uint8_t right_attenuation)
{
	int err;

	err = sci_write(VS1053_REG_VOL,
			((uint16_t)left_attenuation << 8) | right_attenuation);
	if (err) {
		LOG_WRN("Failed to set volume: %d", err);
	}
}

int vs1053_init(void)
{
	uint16_t status;
	int err;

	if (!spi_is_ready_dt(&spi)) {
		LOG_ERR("VS1053 SPI bus not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&dreq_gpio) || !gpio_is_ready_dt(&xdcs_gpio)) {
		LOG_ERR("VS1053 control GPIOs not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&dreq_gpio, GPIO_INPUT);
	err |= gpio_pin_configure_dt(&xdcs_gpio, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("Failed to configure VS1053 GPIOs");
		return -EIO;
	}

	/* No dedicated reset line: the wing ties VS1053 XRESET to the
	 * Feather's shared reset, so the chip is already out of reset by
	 * the time this runs. Just wait for it to signal readiness.
	 */
	err = wait_for_dreq();
	if (err) {
		return err;
	}

	err = sci_write(VS1053_REG_MODE, VS1053_SM_SDINEW | VS1053_SM_RESET);
	if (err) {
		return err;
	}
	k_sleep(K_MSEC(2));

	err = sci_write(VS1053_REG_CLOCKF, VS1053_CLOCKF_VAL);
	if (err) {
		return err;
	}

	err = sci_read(VS1053_REG_STATUS, &status);
	if (err) {
		return err;
	}
	LOG_INF("VS1053 status: 0x%04x", status);

	vs1053_set_volume(VS1053_DEFAULT_VOL_ATTEN, VS1053_DEFAULT_VOL_ATTEN);

	return 0;
}

int vs1053_play_data(const uint8_t *data, size_t len)
{
	struct spi_config data_cfg = spi.config;
	size_t offset = 0;

	data_cfg.frequency = VS1053_DATA_SPI_HZ;
	memset(&data_cfg.cs, 0, sizeof(data_cfg.cs));

	while (offset < len) {
		size_t chunk = MIN(VS1053_SDI_BLOCK_SIZE, len - offset);
		struct spi_buf buf = { .buf = (void *)&data[offset], .len = chunk };
		struct spi_buf_set tx = { .buffers = &buf, .count = 1 };
		int err;

		err = wait_for_dreq();
		if (err) {
			return err;
		}

		gpio_pin_set_dt(&xdcs_gpio, 1);
		err = spi_write(spi.bus, &data_cfg, &tx);
		gpio_pin_set_dt(&xdcs_gpio, 0);
		if (err) {
			LOG_ERR("SDI write failed: %d", err);
			return err;
		}

		offset += chunk;
	}

	return 0;
}

void vs1053_stop(void)
{
	static const uint8_t zero_block[VS1053_SDI_BLOCK_SIZE];
	uint16_t mode;
	/* Datasheet recommends feeding zeros until SM_CANCEL self-clears;
	 * cap the attempts so a stuck decoder can't hang this call.
	 */
	int attempts_left = 64;

	if (sci_read(VS1053_REG_MODE, &mode)) {
		return;
	}

	if (sci_write(VS1053_REG_MODE, mode | VS1053_SM_CANCEL)) {
		return;
	}

	while (attempts_left-- > 0) {
		if (vs1053_play_data(zero_block, sizeof(zero_block))) {
			break;
		}

		if (sci_read(VS1053_REG_MODE, &mode)) {
			break;
		}

		if (!(mode & VS1053_SM_CANCEL)) {
			return;
		}
	}

	LOG_WRN("VS1053 did not clear SM_CANCEL, forcing a soft reset");
	(void)sci_write(VS1053_REG_MODE, VS1053_SM_SDINEW | VS1053_SM_RESET);
	k_sleep(K_MSEC(2));
	(void)sci_write(VS1053_REG_CLOCKF, VS1053_CLOCKF_VAL);
}
