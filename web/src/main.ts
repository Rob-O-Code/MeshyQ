import { ControlPort } from "./serial/controlPort";
import { TerminalPort } from "./serial/terminalPort";
import type { WebApiEvent } from "./serial/types";

const app = document.querySelector<HTMLDivElement>("#app")!;

app.innerHTML = `
  <h1>MeshyQ</h1>
  <p>
    <button id="connect-control">Connect Control Link</button>
    <button id="connect-terminal">Connect Debug Terminal</button>
  </p>
  <pre id="log"></pre>
`;

const logEl = document.querySelector<HTMLPreElement>("#log")!;

function log(line: string): void {
  logEl.textContent += line + "\n";
}

if (!("serial" in navigator)) {
  log("Web Serial is not available in this browser.");
} else {
  const controlPort = new ControlPort((event: WebApiEvent) => {
    log(`event: ${JSON.stringify(event)}`);
  });

  document.querySelector<HTMLButtonElement>("#connect-control")!.addEventListener("click", () => {
    void (async () => {
      try {
        await controlPort.connect();
        log("Control link connected.");
        const status = await controlPort.request("status");
        log(`status: ${JSON.stringify(status)}`);
      } catch (err) {
        log(`Failed to connect control link: ${(err as Error).message}`);
      }
    })();
  });

  const terminalPort = new TerminalPort();

  document.querySelector<HTMLButtonElement>("#connect-terminal")!.addEventListener("click", () => {
    void (async () => {
      try {
        await terminalPort.connect();
        log("Debug terminal connected (raw byte access only; terminal UI is future work).");
      } catch (err) {
        log(`Failed to connect debug terminal: ${(err as Error).message}`);
      }
    })();
  });
}
