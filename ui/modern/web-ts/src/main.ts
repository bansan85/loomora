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
 *
 * Project probe modes (spec 002, contracts/ui-validate-project.md): index.html?validate_project=<name>
 * or index.html?roundtrip_project=<name>, where <name> is a .loomora file next to the page. The page
 * fetches it, hands it to the backend as one buffer (loomora_project_open_memory) and writes the one
 * result line, identical to `loomora validate --quiet` (the round trip: "roundtrip ok " + the line of
 * the copy), in <pre id="validate-project"> and to console.log. The pre has a data-status attribute
 * with the exit status of the command line (0 valid, 1 error, 2 file unreadable or usage error) and
 * document.title becomes LOOMORA_VALIDATE_DONE, or "LOOMORA_VALIDATE_ERROR: <message>". Two modes, or
 * a mode next to probe=1 or with no name, is a usage error: status 2 and no line.
 */
import { CORE_WASM, FAMILY_WEB, UI_MODERN, loadNative } from "./native.js";
import type { Native, ProjectApi } from "./native.js";

const MAX_CHARS = 1000; /* the field limit of ui-contract.md, in UTF-16 units */
const LOAD_TIMEOUT_MS = 60000;

/* LOOMORA_ERR_* of loomora.h and loomora_project.h, LOOMORA_PROFILE_* of loomora_project.h */
const ERR_INVALID = -3;
const ERR_INTERNAL = -4;
const ERR_IO = -10;
const ERR_NO_MEMORY = -16;
const ERR_TOO_LARGE_FOR_PLATFORM = -22;
const PROFILE_FULL = 1;
const PROFILE_LIGHT = 2;
/* The timestamp of the copy: the round trip checks the structure of the copy, not a date, and the page
   reads no clock and parses no part of the project. One fixed valid value. */
const COPY_MODIFIED_AT = "1970-01-01T00:00:00Z";

/** A project probe mode asked for by the URL, with the name of the bundled file as given. */
interface ProjectRequest {
  readonly mode: "validate" | "roundtrip";
  readonly name: string;
}

/** The line of a project mode and its exit status (0, 1 or 2 as the command line's). */
interface ProjectResult {
  readonly line: string;
  readonly status: number;
}

const startTime = Date.now();
let probeMode = false;
let projectRequested = false; /* validate_project or roundtrip_project is in the URL */
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

/* ---- project probe modes ------------------------------------------------------------------ */

/* A failure that leaves nothing to read (I/O, memory, bad argument): status 2, as the command. */
function unreadable(rc: number): boolean {
  return (
    rc === ERR_IO || rc === ERR_NO_MEMORY || rc === ERR_INVALID || rc === ERR_INTERNAL ||
    rc === ERR_TOO_LARGE_FOR_PLATFORM
  );
}

function plural(count: number, noun: string): string {
  return `${count} ${noun}${count === 1 ? "" : "s"}`;
}

/* The summary line of `loomora validate --quiet` for a file: "OK name: 0 errors, 0 warnings",
   "FAIL name: N errors, M warnings" or "FAIL name: error code" (the failure left no error in the
   report), with the exit status of the command. The counts come from the report (pointer 0: none). */
function summaryLine(api: ProjectApi, name: string, rc: number, report: number): ProjectResult {
  const errors = api.reportErrorCount(report);
  const warnings = api.reportCount(report) - errors;
  const codeOnly = rc !== 0 && errors <= 0;
  const status = rc !== 0 && unreadable(rc) ? 2 : rc !== 0 || errors > 0 ? 1 : 0;
  const counts = codeOnly ? `error ${rc}` : `${plural(errors, "error")}, ${plural(warnings, "warning")}`;
  return { line: `${status === 0 ? "OK" : "FAIL"} ${name}: ${counts}`, status };
}

/* The round trip of an open project: a copy of every result in memory, validated again. */
function roundTrip(api: ProjectApi, name: string, project: number): ProjectResult {
  const copy = api.exportCopy(project, api.isLight(project) === 1 ? PROFILE_LIGHT : PROFILE_FULL, COPY_MODIFIED_AT);
  if (copy.rc !== 0) {
    return { line: `roundtrip FAIL export error ${copy.rc}`, status: 1 };
  }
  const validated = api.validate(copy.data, copy.size);
  const summary = summaryLine(api, name, validated.rc, validated.report);
  api.reportFree(validated.report);
  api.release(copy.data);
  const ok = summary.status === 0;
  return { line: `${ok ? "roundtrip ok" : "roundtrip FAIL"} ${summary.line}`, status: ok ? 0 : 1 };
}

/* The line of the mode for the file `bytes` (null when the page could not read it). The whole file is
   handed to the backend as one buffer (loomora_project_open_memory). */
function projectResult(api: ProjectApi, request: ProjectRequest, bytes: ArrayBuffer | null): ProjectResult {
  if (bytes === null) {
    return { line: `FAIL ${request.name}: error ${ERR_IO}`, status: 2 };
  }
  const pointer = api.alloc(new Uint8Array(bytes));
  if (pointer === 0) {
    return { line: `FAIL ${request.name}: error ${ERR_NO_MEMORY}`, status: 2 };
  }
  const opened = api.open(pointer, bytes.byteLength);
  const summary = summaryLine(api, request.name, opened.rc, opened.report);
  api.reportFree(opened.report);
  const result = opened.rc !== 0 || request.mode === "validate" ? summary : roundTrip(api, request.name, opened.project);
  api.close(opened.project);
  api.release(pointer); /* the buffer stays valid until the project is closed */
  return result;
}

/* A file next to the page, as bytes; null when it cannot be read. The name is percent-encoded as one
   path segment, so that it cannot leave the page's directory. */
async function fetchBundled(name: string): Promise<ArrayBuffer | null> {
  try {
    const response = await fetch(encodeURIComponent(name));
    return response.ok ? await response.arrayBuffer() : null;
  } catch {
    return null;
  }
}

/* Writes the line (when there is one) in <pre id="validate-project"> and to console.log, puts the
   status in its data-status attribute, and sets document.title to LOOMORA_VALIDATE_DONE. */
function writeProject(line: string, status: number): void {
  const element = byId("validate-project");
  if (line !== "") {
    element.textContent = line;
    element.hidden = false;
    console.log(line);
  }
  element.setAttribute("data-status", String(status));
  document.title = "LOOMORA_VALIDATE_DONE";
}

/* `request` is null for a usage error: status 2 and no line. */
async function runProjectMode(api: ProjectApi, request: ProjectRequest | null): Promise<void> {
  if (request === null) {
    writeProject("", 2);
    return;
  }
  const result = projectResult(api, request, await fetchBundled(request.name));
  writeProject(result.line, result.status);
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
  if (projectRequested) {
    document.title = "LOOMORA_VALIDATE_ERROR: " + message;
    document.getElementById("validate-project")?.setAttribute("data-status", "2");
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
  projectRequested = /[?&](validate_project|roundtrip_project)([=&]|$)/.test(window.location.search);
  const params = parseQuery(window.location.search);
  /* A project mode is exclusive with the probe and with the other project mode: two of them, or a
     missing name, is a usage error (request null): status 2, and nothing is written. */
  let request: ProjectRequest | null = null;
  if (projectRequested) {
    const validateName = params.get("validate_project");
    const roundtripName = params.get("roundtrip_project");
    const name = validateName ?? roundtripName;
    if (!probeMode && (validateName === undefined || roundtripName === undefined) && name !== undefined && name !== "") {
      request = { mode: validateName !== undefined ? "validate" : "roundtrip", name };
    }
    probeMode = false; /* the probe record is not written next to a project mode */
  }
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
  if (projectRequested) {
    await runProjectMode(core.project, request);
  }
  finished = true;
  clearTimeout(timer);
}

start().catch(fail);
