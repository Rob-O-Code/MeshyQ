import type { WebApiArgs, WebApiEvent, WebApiMessage, WebApiRequest, WebApiResponse } from "./types";
import { isWebApiEvent } from "./types";

/** MeshyQ's USB VID/PID (firmware/src/usb_id.c), shared by both CDC-ACM interfaces. */
const MESHYQ_FILTERS: SerialPortFilter[] = [{ usbVendorId: 0x2fe3, usbProductId: 0x0004 }];

/**
 * Web Serial can't tell JS which of two identically-VID/PID'd ports is which
 * (SerialPort.getInfo() has no interface number), so instead of trusting
 * whichever port the user/browser picked, every connection attempt is
 * verified with a "ping" round trip. If nothing valid comes back in time,
 * this is almost certainly the raw shell/console port, not the control port.
 */
const HANDSHAKE_TIMEOUT_MS = 300;
const DEFAULT_REQUEST_TIMEOUT_MS = 5000;

type PendingRequest = {
  resolve: (response: WebApiResponse) => void;
  reject: (error: Error) => void;
};

export type WebApiEventHandler = (event: WebApiEvent) => void;

export class ControlPort {
  private port: SerialPort | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  private readLoopPromise: Promise<void> | null = null;
  private lineBuffer = "";
  private nextId = 1;
  private pending = new Map<number, PendingRequest>();

  constructor(private readonly onEvent: WebApiEventHandler) {}

  get connected(): boolean {
    return this.port !== null;
  }

  /**
   * Connects to the control port. Tries previously-authorized ports first
   * (no prompt), then falls back to asking the user to pick one. Every
   * candidate is verified with the ping handshake before being accepted.
   */
  async connect(): Promise<void> {
    for (const candidate of await navigator.serial.getPorts()) {
      if (await this.tryConnect(candidate)) {
        return;
      }
    }

    const requested = await navigator.serial.requestPort({ filters: MESHYQ_FILTERS });
    if (await this.tryConnect(requested)) {
      return;
    }

    await this.close();
    throw new Error(
      'That port didn\'t respond like the MeshyQ control port. If your browser offered two ports for this device, please reconnect and choose the other one.',
    );
  }

  private async tryConnect(port: SerialPort): Promise<boolean> {
    try {
      await port.open({ baudRate: 115200 });
    } catch {
      return false;
    }

    this.port = port;
    this.attachStreams();

    try {
      await this.request("ping", undefined, HANDSHAKE_TIMEOUT_MS);
      return true;
    } catch {
      await this.close();
      return false;
    }
  }

  private attachStreams(): void {
    if (!this.port?.readable || !this.port.writable) {
      throw new Error("Serial port is not readable/writable");
    }

    this.reader = this.port.readable.getReader();
    this.writer = this.port.writable.getWriter();
    this.readLoopPromise = this.readLoop();
  }

  private async readLoop(): Promise<void> {
    const decoder = new TextDecoder();

    try {
      while (this.reader) {
        const { value, done } = await this.reader.read();
        if (done) {
          break;
        }
        this.lineBuffer += decoder.decode(value, { stream: true });
        this.drainLines();
      }
    } catch {
      // Port disconnected or closed out from under us; nothing more to read.
    }
  }

  private drainLines(): void {
    let newlineIndex: number;

    while ((newlineIndex = this.lineBuffer.indexOf("\n")) >= 0) {
      const line = this.lineBuffer.slice(0, newlineIndex).trim();
      this.lineBuffer = this.lineBuffer.slice(newlineIndex + 1);
      if (line) {
        this.handleLine(line);
      }
    }
  }

  private handleLine(line: string): void {
    let msg: WebApiMessage;

    try {
      msg = JSON.parse(line) as WebApiMessage;
    } catch {
      // Not JSON: this is never a valid message from the control port, so
      // either it's the wrong port or a framing glitch. Ignore either way.
      return;
    }

    if (isWebApiEvent(msg)) {
      this.onEvent(msg);
      return;
    }

    const pending = this.pending.get(msg.id);
    if (pending) {
      this.pending.delete(msg.id);
      pending.resolve(msg);
    }
  }

  /** Sends a command and resolves with the matching response (by "id"). */
  async request(
    cmd: string,
    args?: WebApiArgs,
    timeoutMs = DEFAULT_REQUEST_TIMEOUT_MS,
  ): Promise<WebApiResponse> {
    if (!this.writer) {
      throw new Error("Not connected");
    }

    const id = this.nextId++;
    const req: WebApiRequest = { v: 1, id, cmd, ...(args ? { args } : {}) };

    const result = new Promise<WebApiResponse>((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
    });

    const timeout = new Promise<never>((_resolve, reject) => {
      setTimeout(() => {
        if (this.pending.delete(id)) {
          reject(new Error(`Timed out waiting for a response to "${cmd}"`));
        }
      }, timeoutMs);
    });

    await this.writer.write(new TextEncoder().encode(JSON.stringify(req) + "\n"));

    return Promise.race([result, timeout]);
  }

  async close(): Promise<void> {
    for (const { reject } of this.pending.values()) {
      reject(new Error("Port closed"));
    }
    this.pending.clear();

    if (this.reader) {
      try {
        await this.reader.cancel();
      } catch {
        /* already closed */
      }
      this.reader.releaseLock();
      this.reader = null;
    }
    await this.readLoopPromise;
    this.readLoopPromise = null;

    if (this.writer) {
      this.writer.releaseLock();
      this.writer = null;
    }

    if (this.port) {
      try {
        await this.port.close();
      } catch {
        /* already closed */
      }
      this.port = null;
    }
  }
}
