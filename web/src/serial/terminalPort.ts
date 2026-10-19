/**
 * Thin raw byte-stream wrapper around the device's existing interactive
 * shell/console CDC-ACM interface. No framing, no parsing -- this is meant
 * to be handed to a terminal emulator (e.g. xterm.js) in a future pass, kept
 * fully separate from ControlPort's structured JSON protocol. See the
 * second connect action in main.ts.
 */

const MESHYQ_FILTERS: SerialPortFilter[] = [{ usbVendorId: 0x2fe3, usbProductId: 0x0004 }];

export class TerminalPort {
  private port: SerialPort | null = null;

  get connected(): boolean {
    return this.port !== null;
  }

  async connect(): Promise<void> {
    const port = await navigator.serial.requestPort({ filters: MESHYQ_FILTERS });
    await port.open({ baudRate: 115200 });
    this.port = port;
  }

  get readable(): ReadableStream<Uint8Array> | null {
    return this.port?.readable ?? null;
  }

  get writable(): WritableStream<Uint8Array> | null {
    return this.port?.writable ?? null;
  }

  async close(): Promise<void> {
    if (this.port) {
      await this.port.close();
      this.port = null;
    }
  }
}
