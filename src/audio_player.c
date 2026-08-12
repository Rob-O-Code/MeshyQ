#include "audio_player.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <hal/nrf_clock.h>
#include <nrfx_clock.h>
#include <nrfx_i2s.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "cs47l63.h"
#include "cs47l63_comm.h"
#include "cs47l63_spec.h"

LOG_MODULE_REGISTER(audio_player, CONFIG_LOG_DEFAULT_LEVEL);

#define HFCLKAUDIO_12_288_MHZ 0x9BA6
#define CODEC_BUSY_WAIT_REG 0x0001
#define CODEC_BUSY_WAIT_US_1000 1000
#define CODEC_BUSY_WAIT_US_3000 3000
#define CODEC_OUT_VOLUME_DEFAULT 0x62
#define CODEC_OUT_VOLUME_MAX_DB 64
#define CODEC_SOFT_RESET_VAL 0x5A000000

#define DING_SAMPLE_RATE_HZ 48000
#define DING_BLOCK_FRAMES 480
#define DING_THREAD_STACK_SIZE 3072
#define DING_THREAD_PRIO 7

#define I2S_NODE DT_NODELABEL(i2s0)

PINCTRL_DT_DEFINE(I2S_NODE);

static const uint32_t clock_configuration[][2] = {
	{ CS47L63_SAMPLE_RATE3, 0x0012 },
	{ CS47L63_SAMPLE_RATE2, 0x0002 },
	{ CS47L63_SAMPLE_RATE1, 0x0003 },
	{ CS47L63_SYSTEM_CLOCK1, 0x034C },
	{ CS47L63_ASYNC_CLOCK1, 0x034C },
	{ CS47L63_FLL1_CONTROL2, 0x88200008 },
	{ CS47L63_FLL1_CONTROL3, 0x10000 },
	{ CS47L63_FLL1_GPIO_CLOCK, 0x0005 },
	{ CS47L63_FLL1_CONTROL1, 0x0001 },
};

static const uint32_t gpio_configuration[][2] = {
	{ CS47L63_GPIO6_CTRL1, 0x61000001 },
	{ CS47L63_GPIO7_CTRL1, 0x61000001 },
	{ CS47L63_GPIO8_CTRL1, 0x61000001 },
	{ CS47L63_GPIO10_CTRL1, 0x41008001 },
};

static const uint32_t asp1_enable[][2] = {
	{ CS47L63_GPIO1_CTRL1, 0x61000000 },
	{ CS47L63_GPIO2_CTRL1, 0xE1000000 },
	{ CS47L63_GPIO3_CTRL1, 0xE1000000 },
	{ CS47L63_GPIO4_CTRL1, 0xE1000000 },
	{ CS47L63_GPIO5_CTRL1, 0x61000001 },
	{ CS47L63_SAMPLE_RATE1, 0x000000003 },
	{ CS47L63_SAMPLE_RATE2, 0 },
	{ CS47L63_SAMPLE_RATE3, 0 },
	{ CS47L63_SAMPLE_RATE4, 0 },
	{ CS47L63_ASP1_CONTROL2, 0x10100200 },
	{ CS47L63_ASP1_CONTROL3, 0x0000 },
	{ CS47L63_ASP1_DATA_CONTROL1, 0x0020 },
	{ CS47L63_ASP1_DATA_CONTROL5, 0x0020 },
	{ CS47L63_ASP1_ENABLES1, 0x30003 },
};

static const uint32_t output_enable[][2] = {
	{ CS47L63_OUTPUT_ENABLE_1, 0x0002 },
	{ CS47L63_OUT1L_INPUT1, 0x800020 },
	{ CS47L63_OUT1L_INPUT2, 0x800021 },
};

static const uint32_t fll_toggle[][2] = {
	{ CS47L63_FLL1_CONTROL1, 0x0000 },
	{ CODEC_BUSY_WAIT_REG, CODEC_BUSY_WAIT_US_1000 },
	{ CS47L63_FLL1_CONTROL1, 0x0001 },
};

static const uint32_t soft_reset[][2] = {
	{ CS47L63_SFT_RESET, CODEC_SOFT_RESET_VAL },
	{ CODEC_BUSY_WAIT_REG, CODEC_BUSY_WAIT_US_3000 },
};

static const unsigned char ding_pcm[] = {
#include <ding_pcm.inc>
};

static nrfx_i2s_t i2s_inst = NRFX_I2S_INSTANCE(NRF_I2S0);

static nrfx_i2s_config_t i2s_cfg = {
	.skip_gpio_cfg = true,
	.skip_psel_cfg = true,
	.irq_priority = DT_IRQ(I2S_NODE, priority),
	.mode = NRF_I2S_MODE_MASTER,
	.format = NRF_I2S_FORMAT_I2S,
	.alignment = NRF_I2S_ALIGN_LEFT,
	.prescalers = {
		.ratio = NRF_I2S_RATIO_128X,
		.mck_setup = 0x66666000,
		.enable_bypass = false,
	},
	.sample_width = NRF_I2S_SWIDTH_16BIT,
	.channels = NRF_I2S_CHANNELS_STEREO,
	.clksrc = NRF_I2S_CLKSRC_ACLK,
};

K_THREAD_STACK_DEFINE(audio_thread_stack, DING_THREAD_STACK_SIZE);

static struct {
	struct k_sem play_request;
	struct k_sem play_finished;
	struct k_mutex lock;
	struct k_thread thread;
	k_tid_t thread_id;
	cs47l63_t codec;
	size_t pcm_offset;
	bool initialized;
	bool busy;
	bool stop_pending;
	bool codec_running;
	uint32_t tx_buf[2][DING_BLOCK_FRAMES];
	uint8_t next_tx_buf;
} player;

static int codec_write_sequence(const uint32_t sequence[][2], size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (sequence[i][0] == CODEC_BUSY_WAIT_REG) {
			k_busy_wait(sequence[i][1]);
			continue;
		}

		if (cs47l63_write_reg(&player.codec, sequence[i][0], sequence[i][1])) {
			return -EIO;
		}
	}

	return 0;
}

static int codec_configure(void)
{
	int err;

	err = codec_write_sequence(soft_reset, ARRAY_SIZE(soft_reset));
	if (err) {
		return err;
	}

	player.codec.state = CS47L63_STATE_STANDBY;

	err = codec_write_sequence(clock_configuration, ARRAY_SIZE(clock_configuration));
	if (err) {
		return err;
	}

	err = codec_write_sequence(gpio_configuration, ARRAY_SIZE(gpio_configuration));
	if (err) {
		return err;
	}

	err = codec_write_sequence(asp1_enable, ARRAY_SIZE(asp1_enable));
	if (err) {
		return err;
	}

	err = codec_write_sequence(output_enable, ARRAY_SIZE(output_enable));
	if (err) {
		return err;
	}

	err = cs47l63_write_reg(&player.codec, CS47L63_OUT1L_VOLUME_1,
			       CODEC_OUT_VOLUME_DEFAULT | CS47L63_OUT_VU);
	if (err) {
		return -EIO;
	}

	return 0;
}

static int audio_clock_init_local(void)
{
	int err;

	if (!nrfx_clock_init_check()) {
		err = nrfx_clock_init(NULL);
		if (err && err != -EALREADY) {
			return err;
		}
	}

#if NRF_CLOCK_HAS_HFCLKAUDIO
	err = nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK, NRF_CLOCK_HFCLK_DIV_1);
	if (err && err != -EALREADY) {
		return err;
	}

	nrfx_clock_hfclkaudio_config_set(HFCLKAUDIO_12_288_MHZ);
	NRF_CLOCK->TASKS_HFCLKAUDIOSTART = 1;
	while (!NRF_CLOCK_EVENT_HFCLKAUDIOSTARTED) {
		k_sleep(K_MSEC(1));
	}

	return 0;
#else
	return -ENOTSUP;
#endif
}

static bool fill_tx_buffer(uint32_t *buffer)
{
	const int16_t *samples = (const int16_t *)ding_pcm;
	const size_t sample_count = sizeof(ding_pcm) / sizeof(int16_t);
	bool has_more = false;

	for (size_t i = 0; i < DING_BLOCK_FRAMES; i++) {
		int16_t mono_sample = 0;

		if (player.pcm_offset < sample_count) {
			mono_sample = samples[player.pcm_offset++];
			has_more = (player.pcm_offset < sample_count);
		}

		buffer[i] = ((uint32_t)(uint16_t)mono_sample << 16) | (uint16_t)mono_sample;
	}

	return has_more;
}

static void i2s_handler(nrfx_i2s_buffers_t const *released_bufs, uint32_t status)
{
	ARG_UNUSED(released_bufs);

	if ((status & NRFX_I2S_STATUS_NEXT_BUFFERS_NEEDED) != 0U) {
		nrfx_i2s_buffers_t next = {
			.p_rx_buffer = NULL,
			.p_tx_buffer = player.tx_buf[player.next_tx_buf],
			.buffer_size = DING_BLOCK_FRAMES,
		};
		int err;
		bool has_more;

		if (player.stop_pending) {
			nrfx_i2s_stop(&i2s_inst);
		} else {
			has_more = fill_tx_buffer(player.tx_buf[player.next_tx_buf]);
			if (!has_more) {
				player.stop_pending = true;
			}

			err = nrfx_i2s_next_buffers_set(&i2s_inst, &next);
			if (err) {
				player.stop_pending = true;
				nrfx_i2s_stop(&i2s_inst);
			}

			player.next_tx_buf ^= 1U;
		}
	}

	if ((status & NRFX_I2S_STATUS_TRANSFER_STOPPED) != 0U) {
		k_sem_give(&player.play_finished);
	}
}

static int i2s_prepare(void)
{
	int err;

	err = pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(I2S_NODE), PINCTRL_STATE_DEFAULT);
	if (err) {
		return err;
	}

	if (!nrfx_i2s_init_check(&i2s_inst)) {
		IRQ_CONNECT(DT_IRQN(I2S_NODE), DT_IRQ(I2S_NODE, priority), nrfx_i2s_irq_handler,
			    &i2s_inst, 0);
		irq_enable(DT_IRQN(I2S_NODE));

		err = nrfx_i2s_init(&i2s_inst, &i2s_cfg, i2s_handler);
		if (err && err != -EALREADY) {
			return err;
		}
	}

	return 0;
}

static int playback_start(void)
{
	nrfx_i2s_buffers_t initial = {
		.p_rx_buffer = NULL,
		.p_tx_buffer = player.tx_buf[0],
		.buffer_size = DING_BLOCK_FRAMES,
	};
	int err;

	player.pcm_offset = 0;
	player.stop_pending = false;
	player.next_tx_buf = 1;
	(void)fill_tx_buffer(player.tx_buf[0]);
	memset(player.tx_buf[1], 0, sizeof(player.tx_buf[1]));

	err = nrfx_i2s_start(&i2s_inst, &initial, 0);
	if (err) {
		return err;
	}

	if (!player.codec_running) {
		err = codec_write_sequence(fll_toggle, ARRAY_SIZE(fll_toggle));
		if (err) {
			nrfx_i2s_stop(&i2s_inst);
			return err;
		}

		player.codec_running = true;
	}

	return 0;
}

static void audio_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		k_sem_take(&player.play_request, K_FOREVER);
		if (playback_start() == 0) {
			k_sem_take(&player.play_finished, K_FOREVER);
		}

		k_mutex_lock(&player.lock, K_FOREVER);
		player.busy = false;
		k_mutex_unlock(&player.lock);
	}
}

int audio_player_init(void)
{
	int err;

	if (player.initialized) {
		return 0;
	}

	k_sem_init(&player.play_request, 0, 1);
	k_sem_init(&player.play_finished, 0, 1);
	k_mutex_init(&player.lock);

	err = audio_clock_init_local();
	if (err) {
		return err;
	}

	err = i2s_prepare();
	if (err) {
		return err;
	}

	err = cs47l63_comm_init(&player.codec);
	if (err) {
		return err;
	}

	err = codec_configure();
	if (err) {
		return err;
	}

	player.thread_id = k_thread_create(&player.thread, audio_thread_stack,
					   K_THREAD_STACK_SIZEOF(audio_thread_stack),
					   audio_thread, NULL, NULL, NULL,
					   K_PRIO_PREEMPT(DING_THREAD_PRIO), 0,
					   K_NO_WAIT);
	(void)k_thread_name_set(player.thread_id, "audio_player");

	player.initialized = true;
	return 0;
}

int audio_player_play_ding(void)
{
	if (!player.initialized) {
		return -ENODEV;
	}

	k_mutex_lock(&player.lock, K_FOREVER);
	if (player.busy) {
		k_mutex_unlock(&player.lock);
		return -EALREADY;
	}

	player.busy = true;
	k_mutex_unlock(&player.lock);

	k_sem_give(&player.play_request);
	return 0;
}