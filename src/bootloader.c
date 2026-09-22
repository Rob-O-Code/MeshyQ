/** @file
 *  @brief Shell command to reboot into the Adafruit UF2 bootloader.
 *
 *  The board ships with Adafruit's UF2 bootloader (see
 *  boards/adafruit_feather_nrf52840_nrf52840_uf2.conf), which normally only
 *  exposes its mass-storage drive for drag-and-drop flashing after a
 *  physical double-tap of the reset button. That bootloader also enters the
 *  same mode on a warm boot if it finds a magic value in POWER's GPREGRET
 *  retention register, which a system reset leaves untouched. This lets a
 *  shell command over the existing CDC-ACM console (usb_id.c) trigger it
 *  without touching the board.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>

/* Adafruit_nRF52_Bootloader's DFU_MAGIC_UF2_RESET: tells the bootloader to
 * stay in UF2 mass-storage mode instead of jumping to the application.
 */
#define ADAFRUIT_UF2_DFU_MAGIC 0x57

static int cmd_bootloader(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Rebooting into UF2 bootloader...");
	k_msleep(100); /* let the console output above drain before reset */

	nrf_power_gpregret_set(NRF_POWER, 0, ADAFRUIT_UF2_DFU_MAGIC);
	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

SHELL_CMD_REGISTER(bootloader, NULL,
		    "Reboot into the UF2 mass-storage bootloader", cmd_bootloader);
