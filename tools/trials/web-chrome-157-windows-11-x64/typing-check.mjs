#!/usr/bin/env node
// Typing check of the Web Modern page in Chrome (headless=new) through the DevTools protocol.
// Tooling, not evidence: its log is the evidence (typing-check-log.txt).
//
//   node typing-check.mjs <chrome.exe> <dist folder> [--port N]
//
// Serves <dist folder> on 127.0.0.1 (a free port) from this process, starts Chrome with a new
// throw-away --user-data-dir (a folder of the system temp folder, deleted at the end; the profile of
// the user is never opened) and --remote-debugging-port=0, then:
//   1. opens index.html (no probe mode) and waits for the default greeting "Bonjour Coucou";
//   2. focuses the textarea, selects its content and types "Marie" with real key events
//      (Input.dispatchKeyEvent: keyDown with text, then keyUp, one per character), then waits for
//      the greeting "Bonjour Marie" and reports the time from the last key to the greeting;
//   3. reads the colours of the page with prefers-color-scheme light and dark (Emulation.setEmulatedMedia)
//      and the contrast ratio of the text on the background;
//   4. closes the browser (Browser.close) and counts the chrome.exe processes that still use the
//      profile folder (terminated by PID if any).
import { spawn, spawnSync } from "node:child_process";
import { createServer } from "node:http";
import { mkdtempSync, readFileSync, rmSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, extname } from "node:path";

const [chromeExe, distDir] = process.argv.slice(2);
if (!chromeExe || !distDir) {
  console.error("usage: node typing-check.mjs <chrome.exe> <dist folder>");
  process.exit(2);
}
const log = (text) => console.log(text);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const TYPES = { ".html": "text/html; charset=utf-8", ".js": "application/javascript", ".wasm": "application/wasm" };

const requests = [];
const server = createServer((req, res) => {
  const name = new URL(req.url, "http://x").pathname.replace(/^\/$/, "/index.html").slice(1);
  const file = join(distDir, name);
  if (name.includes("/") || !existsSync(file)) {
    res.writeHead(404).end("not found");
    requests.push(`${req.method} /${name} -> 404`);
    return;
  }
  const body = readFileSync(file);
  res.writeHead(200, { "Content-Type": TYPES[extname(file)] ?? "application/octet-stream", "Cache-Control": "no-store" });
  res.end(body);
  requests.push(`${req.method} /${name} -> 200 (${body.length} bytes)`);
});
await new Promise((r) => server.listen(0, "127.0.0.1", r));
const port = server.address().port;
const url = `http://127.0.0.1:${port}/index.html`;

const profile = mkdtempSync(join(tmpdir(), "loomora-chrome-typing-"));
const chrome = spawn(chromeExe, ["--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
  "--disable-extensions", "--disable-component-update", "--disable-background-networking", "--disable-sync",
  "--remote-debugging-port=0", `--user-data-dir=${profile}`, "about:blank"], { stdio: "ignore" });
const exited = new Promise((r) => chrome.on("exit", (code) => r(code)));
const started = Date.now();

let ws;
let nextId = 0;
const pending = new Map();
function send(method, params = {}, sessionId) {
  const id = ++nextId;
  ws.send(JSON.stringify({ id, method, params, sessionId }));
  return new Promise((resolve, reject) => pending.set(id, { resolve, reject, method }));
}

function pids() {
  const script = `Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" | Where-Object { $_.CommandLine -like '*${profile}*' } | ForEach-Object { $_.ProcessId }`;
  const res = spawnSync("powershell", ["-NoProfile", "-Command", script], { encoding: "utf8" });
  return res.stdout.split(/\s+/).filter((w) => /^\d+$/.test(w)).map(Number);
}

function luminance(rgb) {
  const [r, g, b] = rgb.map((v) => {
    const c = v / 255;
    return c <= 0.03928 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
  });
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}
const parseRgb = (css) => css.match(/\d+(\.\d+)?/g).slice(0, 3).map(Number);
function contrast(a, b) {
  const [hi, lo] = [luminance(a), luminance(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
}

let failed = false;
function check(label, ok, detail) {
  log(`${ok ? "ok  " : "FAIL"} ${label}${detail === undefined ? "" : ": " + detail}`);
  if (!ok) failed = true;
}

try {
  let endpoint = null;
  for (let i = 0; i < 200 && endpoint === null; i++) {
    try {
      const [p, path] = readFileSync(join(profile, "DevToolsActivePort"), "utf8").trim().split(/\r?\n/);
      endpoint = `ws://127.0.0.1:${p}${path}`;
    } catch (e) {
      await sleep(50);
    }
  }
  if (endpoint === null) throw new Error("Chrome did not publish its DevTools port");
  ws = new WebSocket(endpoint);
  await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = () => reject(new Error("websocket error")); });
  ws.onmessage = (event) => {
    const msg = JSON.parse(event.data);
    const p = msg.id !== undefined ? pending.get(msg.id) : undefined;
    if (p) {
      pending.delete(msg.id);
      if (msg.error) p.reject(new Error(`${p.method}: ${msg.error.message}`)); else p.resolve(msg.result);
    }
  };
  log(`chrome.exe started, DevTools endpoint ready after ${Date.now() - started} ms (profile ${profile})`);
  const version = await send("Browser.getVersion");
  log(`browser: ${version.product}; user agent: ${version.userAgent}`);

  const { targetInfos } = await send("Target.getTargets");
  const page = targetInfos.find((t) => t.type === "page");
  const { sessionId } = await send("Target.attachToTarget", { targetId: page.targetId, flatten: true });
  const call = (method, params) => send(method, params, sessionId);
  const evaluate = async (expression) => (await call("Runtime.evaluate", { expression, returnByValue: true })).result.value;
  await call("Page.enable");
  await call("Runtime.enable");

  const navigated = Date.now();
  await call("Page.navigate", { url });
  let greeting = "";
  while (Date.now() - navigated < 20000) {
    greeting = (await evaluate("document.getElementById('greeting') && document.getElementById('greeting').textContent")) || "";
    if (greeting !== "") break;
    await sleep(10);
  }
  check("default greeting shown", greeting === "Bonjour Coucou", `${JSON.stringify(greeting)} ${Date.now() - navigated} ms after Page.navigate`);
  check("field prefilled", (await evaluate("document.getElementById('name').value")) === "Coucou");
  check("status line empty once loaded", (await evaluate("document.getElementById('status').textContent")) === "");

  await evaluate("(function () { var f = document.getElementById('name'); f.focus(); f.select(); return document.activeElement === f; })()");
  const keys = { M: 77, a: 65, r: 82, i: 73, e: 69 };
  for (const ch of "Marie") {
    const params = { key: ch, code: "Key" + ch.toUpperCase(), windowsVirtualKeyCode: keys[ch], text: ch, unmodifiedText: ch,
      modifiers: ch === "M" ? 8 : 0 };
    await call("Input.dispatchKeyEvent", { type: "keyDown", ...params });
    await call("Input.dispatchKeyEvent", { type: "keyUp", key: ch, code: params.code, windowsVirtualKeyCode: keys[ch] });
  }
  const typed = Date.now();
  let after = "";
  while (Date.now() - typed < 5000) {
    after = await evaluate("document.getElementById('greeting').textContent");
    if (after === "Bonjour Marie") break;
    await sleep(5);
  }
  check("typed 'Marie' (real key events): field", (await evaluate("document.getElementById('name').value")) === "Marie");
  check("typed 'Marie': greeting", after === "Bonjour Marie", `${JSON.stringify(after)} read ${Date.now() - typed} ms after the last key event`);

  for (const scheme of ["dark", "light"]) {
    await call("Emulation.setEmulatedMedia", { features: [{ name: "prefers-color-scheme", value: scheme }] });
    await sleep(50);
    const media = await evaluate("matchMedia('(prefers-color-scheme: dark)').matches");
    const colours = await evaluate("(function () { var s = getComputedStyle(document.body); var c = getComputedStyle(document.getElementById('greeting')); var t = getComputedStyle(document.getElementById('name')); var k = getComputedStyle(document.querySelector('.card')); return [s.backgroundColor, c.color, t.backgroundColor, t.color, getComputedStyle(document.documentElement).colorScheme, k.backgroundColor]; })()");
    const ratio = contrast(parseRgb(colours[1]), parseRgb(colours[5]));
    const fieldRatio = contrast(parseRgb(colours[3]), parseRgb(colours[2]));
    log(`prefers-color-scheme ${scheme}: matches dark = ${media}; page background ${colours[0]}, card background ${colours[5]}, greeting text ${colours[1]}, field background ${colours[2]}, field text ${colours[3]}, color-scheme ${colours[4]}; greeting on card contrast ${ratio.toFixed(2)}:1, field text on field background ${fieldRatio.toFixed(2)}:1`);
    check(`${scheme} scheme applied by the page`, media === (scheme === "dark") && ratio >= 7 && fieldRatio >= 7);
  }
  await call("Emulation.setEmulatedMedia", { features: [] });

  try { await send("Browser.close"); } catch (e) { /* the connection closes with the browser */ }
  const code = await Promise.race([exited, sleep(15000).then(() => "timeout")]);
  await sleep(500);
  const left = pids();
  for (const pid of left) spawnSync("taskkill", ["/PID", String(pid), "/T", "/F"]);
  check("clean exit", code === 0 && left.length === 0, `exit status ${code}, ${left.length} chrome.exe left on this profile`);
} catch (e) {
  failed = true;
  log("ERROR " + e.message);
  const left = pids();
  for (const pid of left) spawnSync("taskkill", ["/PID", String(pid), "/T", "/F"]);
} finally {
  log("server requests:\n" + requests.map((r) => "  " + r).join("\n"));
  server.close();
  rmSync(profile, { recursive: true, force: true });
}
log(failed ? "typing-check FAILED" : "typing-check passed");
process.exit(failed ? 1 : 0);
