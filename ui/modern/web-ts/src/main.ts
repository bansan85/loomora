/* Loomora Web Modern UI: the page of index.html. No application logic: it loads the backend
 * (native.ts), shows the greeting the backend returns for the text of the field, on load and on
 * every input event, and in probe mode writes the probe record of the backend.
 *
 * Probe mode (ui-contract.md, SC-011), the same query as ui/legacy/web/app.js:
 *   index.html?probe=1&input=<percent-encoded UTF-8>&platform=<platform id>
 *   probe=1      probe mode (any value; the parameter alone also works)
 *   input=TEXT   the text put in the field instead of "Coucou" (cut to 1,000 UTF-16 units)
 *   platform=ID  the platform_id of the probe record (default "web-unknown")
 * The probe record (one ASCII JSON line, loomora_probe_record, ui_variant modern, core cpp-wasm) is
 * written in <pre id="probe"> and to console.log, and document.title becomes LOOMORA_PROBE_DONE, or
 * "LOOMORA_PROBE_ERROR: <message>" when something failed. '+' in a value is a plus sign.
 */
import { CORE_WASM, FAMILY_WEB, UI_MODERN, loadNative } from "./native.js";
import type { Native } from "./native.js";

const MAX_CHARS = 1000; /* the field limit of ui-contract.md, in UTF-16 units */
const LOAD_TIMEOUT_MS = 60000;

const startTime = Date.now();
let probeMode = false;
let finished = false;
let failed = false;
let lastText: string | null = null;
let native: Native | null = null;

function byId<T extends HTMLElement>(id: string): T {
  const element = document.getElementById(id);
  if (element === null) {
    throw new Error("index.html has no element " + id);
  }
  return element as T;
}

/* ---- URL parameters ---------------------------------------------------------------------- */

/* The parameters of a query string; decodeURIComponent, not URLSearchParams, so that '+' stays a
   plus sign as in the Legacy page. Throws URIError on a malformed percent sequence. */
function parseQuery(search: string): Map<string, string> {
  const result = new Map<string, string>();
  const text = search.startsWith("?") ? search.substring(1) : search;
  for (const pair of text.split("&")) {
    if (pair === "") {
      continue;
    }
    const eq = pair.indexOf("=");
    if (eq < 0) {
      result.set(decodeURIComponent(pair), "");
    } else {
      result.set(decodeURIComponent(pair.substring(0, eq)), decodeURIComponent(pair.substring(eq + 1)));
    }
  }
  return result;
}

/* The field limit, never cutting a surrogate pair in two. */
function limitText(text: string): string {
  if (text.length <= MAX_CHARS) {
    return text;
  }
  const last = text.charCodeAt(MAX_CHARS - 1);
  return text.substring(0, last >= 0xd800 && last <= 0xdbff ? MAX_CHARS - 1 : MAX_CHARS);
}

/* ---- probe -------------------------------------------------------------------------------- */

/* Best effort (ui-contract.md): performance.memory where the browser has it (Chrome), in KiB;
   -1 = not measured. It is the JavaScript heap in use, not a peak. */
function memoryKb(): number {
  const memory = (performance as { memory?: { usedJSHeapSize?: number } }).memory;
  if (memory !== undefined && typeof memory.usedJSHeapSize === "number" && memory.usedJSHeapSize > 0) {
    return Math.floor(memory.usedJSHeapSize / 1024);
  }
  return -1;
}

function timestampUtc(): string {
  return new Date().toISOString().replace(/\.\d+Z$/, "Z");
}

function writeProbe(core: Native, platformId: string, eolShown: boolean): void {
  const line = core.probe({
    platformId,
    uiVariant: UI_MODERN,
    coreKind: CORE_WASM,
    input: byId<HTMLTextAreaElement>("name").value,
    displayedText: byId("greeting").textContent ?? "",
    displayed: 1,
    displayMs: Date.now() - startTime,
    rssKb: memoryKb(),
    eolWarningShown: eolShown ? 1 : 0,
    timestampUtc: timestampUtc(),
  });
  const probeEl = byId("probe");
  probeEl.textContent = line;
  probeEl.hidden = false;
  console.log(line);
  document.title = "LOOMORA_PROBE_DONE";
}

/* ---- display ------------------------------------------------------------------------------ */

function showGreeting(core: Native, text: string): void {
  byId("greeting").textContent = core.greet(text);
  lastText = text;
}

function onInput(): void {
  if (native === null || failed) {
    return;
  }
  const text = byId<HTMLTextAreaElement>("name").value;
  if (text !== lastText) {
    try {
      showGreeting(native, text);
    } catch (error) {
      fail(error);
    }
  }
}

function fail(error: unknown): void {
  const message = error instanceof Error ? error.message : String(error);
  if (failed) {
    return;
  }
  failed = true;
  if (probeMode) {
    document.title = "LOOMORA_PROBE_ERROR: " + message;
  }
  const status = document.getElementById("status");
  if (status !== null) {
    status.textContent = "Error: " + message;
  }
  console.log("LOOMORA_ERROR: " + message);
}

/* ---- start -------------------------------------------------------------------------------- */

async function start(): Promise<void> {
  const field = byId<HTMLTextAreaElement>("name");
  /* decided before the parsing, so that a malformed URL is still reported as a probe error */
  probeMode = /[?&]probe([=&]|$)/.test(window.location.search);
  const params = parseQuery(window.location.search);
  if (probeMode) {
    const input = params.get("input");
    if (input !== undefined) {
      field.value = limitText(input);
    }
  }
  const timer = setTimeout(() => {
    if (!finished && !failed) {
      fail("timeout: the backend did not start in " + LOAD_TIMEOUT_MS / 1000 + " s");
    }
  }, LOAD_TIMEOUT_MS);

  const core = await loadNative();
  native = core;
  /* The family is all the page tells the backend about the system: a browser cannot read the
     system version, and the backend answers 0 for the Web family; a 1 would show the warning. */
  const eol = core.isEol(FAMILY_WEB, 0, 0) === 1;
  if (eol) {
    const eolEl = byId("eol");
    eolEl.textContent = core.eolWarning();
    eolEl.hidden = false;
  }
  showGreeting(core, field.value);
  byId("status").textContent = "";
  field.addEventListener("input", onInput);
  if (probeMode) {
    writeProbe(core, params.get("platform") ?? "web-unknown", eol);
  }
  finished = true;
  clearTimeout(timer);
}

start().catch(fail);
