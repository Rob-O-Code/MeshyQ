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
  <p>
    <button id="list-files" disabled>List Files on All Nodes</button>
  </p>
  <div id="files"></div>
  <pre id="log"></pre>
`;

const logEl = document.querySelector<HTMLPreElement>("#log")!;

const filesEl = document.querySelector<HTMLDivElement>("#files")!;
const listButton = document.querySelector<HTMLButtonElement>("#list-files")!;

function log(line: string): void {
  logEl.textContent += line + "\n";
}

/** Broadcasts "play <file>" to the whole mesh; any node with a matching file plays it. */
async function playFile(file: string): Promise<void> {
  try {
    const res = await controlPort?.request("cue_play", { filename: file });
    log(res?.ok ? `Triggered "${file}".` : `cue_play failed: ${res?.error?.message ?? "not connected"}`);
  } catch (err) {
    log(`cue_play failed: ${(err as Error).message}`);
  }
}

/** Renders one node's "list" reply: a comma-separated file list, or "(none)". */
function showNodeFiles(addr: string, reply: string): void {
  const files = reply === "(none)" ? [] : reply.split(",").map((f) => f.trim()).filter(Boolean);

  const section = document.createElement("section");
  const heading = document.createElement("h3");
  heading.textContent = `Node ${addr}`;
  section.append(heading);

  if (files.length === 0) {
    section.append("No playable files.");
  } else {
    const ul = document.createElement("ul");
    for (const file of files) {
      const li = document.createElement("li");
      const button = document.createElement("button");
      button.textContent = file;
      button.addEventListener("click", () => void playFile(file));
      li.append(button);
      ul.append(li);
    }
    section.append(ul);
  }

  filesEl.append(section);
}

let controlPort: ControlPort | undefined;

if (!("serial" in navigator)) {
  log("Web Serial is not available in this browser.");
} else {
  const port = new ControlPort((event: WebApiEvent) => {
    log(`event: ${JSON.stringify(event)}`);
    // Every node answers a mesh "list" broadcast with a chat reply.
    if (event.event === "chat_message_reply" && event.addr && event.text !== undefined) {
      showNodeFiles(event.addr, event.text);
    }
  });

  controlPort = port;

  document.querySelector<HTMLButtonElement>("#connect-control")!.addEventListener("click", () => {
    void (async () => {
      try {
        await port.connect();
        log("Control link connected.");
        const status = await port.request("status");
        log(`status: ${JSON.stringify(status)}`);
        listButton.disabled = false;
      } catch (err) {
        log(`Failed to connect control link: ${(err as Error).message}`);
      }
    })();
  });

  listButton.addEventListener("click", () => {
    void (async () => {
      filesEl.textContent = "";
      try {
        const res = await port.request("cue_list");
        if (!res.ok) {
          log(`cue_list failed: ${res.error?.message ?? "unknown error"}`);
        } else {
          log("List request broadcast; waiting for node replies...");
        }
      } catch (err) {
        log(`cue_list failed: ${(err as Error).message}`);
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
