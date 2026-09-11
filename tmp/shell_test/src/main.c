/*
 * Bisection step 1: confirm the shell comes up over USB CDC-ACM on the
 * Feather /uf2 target with nothing else (no BT, no SPI, no SD, no audio)
 * running. If this is stable, the next step adds Bluetooth/mesh; if that
 * one is also stable, the step after adds the SD/VS1053 code.
 */
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

static int cmd_ping(const struct shell *shell, size_t argc, char *argv[])
{
	shell_print(shell, "pong");
	return 0;
}

SHELL_CMD_ARG_REGISTER(ping, NULL, "Reply with pong, to confirm the shell is alive", cmd_ping, 1,
		       0);

int main(void)
{
	printk("shell_test ready\n");
	return 0;
}
