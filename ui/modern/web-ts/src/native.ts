/* The backend of the Web Modern UI: libloomora (C++11, built by Emscripten as loomora.js +
 * loomora.wasm) seen through ccall / cwrap. No logic of its own: this file loads the module, copies
 * UTF-8 bytes in and out of its memory, and calls the six functions of the C API
 * (bridge/wasm/README.md). The greeting, the probe line and the end-of-life test all come from the
 * backend.
 *
 * Loading: loomora.js is the classic script of Emscripten (it defines the global createLoomora);
 * the module reads loomora.wasm through WebAssembly.instantiateStreaming, with the ArrayBuffer
 * form (WebAssembly.instantiate) when the server does not send application/wasm or the browser has
 * no instantiateStreaming. Only the WebAssembly build exists here: the 64-bit rss_kb of
 * loomora_probe_record is a BigInt.
 */

const ABI_VERSION = 1;
const GREET_CAP = 4200; /* 4,000 bytes at most (1,000 UTF-16 units) + the prefix + room */
const PROBE_CAP = 16384; /* the record is ASCII, at most about 14,000 bytes */

/** Values of loomora.h. */
export const FAMILY_WEB = 6;
export const UI_MODERN = 2;
export const CORE_WASM = 2;

type CFunction = (...args: Array<number | bigint>) => number;

/** The part of the Emscripten module object that this file uses (bridge/wasm/runtime-methods.txt). */
interface EmscriptenModule {
  /** The result of a function that returns an int64 is a BigInt in the WebAssembly build. */
  ccall(name: string, returnType: "number", argTypes: string[], args: Array<number | bigint>): number;
  ccall(name: string, returnType: null, argTypes: string[], args: Array<number | bigint>): void;
  cwrap(name: string, returnType: "number", argTypes: string[]): CFunction;
  stackSave(): number;
  stackAlloc(size: number): number;
  stackRestore(pointer: number): void;
  UTF8ToString(pointer: number): string;
  HEAPU8: Uint8Array;
}

interface EmscriptenOptions {
  instantiateWasm(
    imports: WebAssembly.Imports,
    done: (instance: WebAssembly.Instance, module?: WebAssembly.Module) => void,
  ): Record<string, never>;
}

type CreateModule = (options: EmscriptenOptions) => Promise<EmscriptenModule>;

/** Everything the page asks of the backend. */
export interface Native {
  /** The greeting of the backend for `text`; throws if the backend refuses. */
  greet(text: string): string;
  /** loomora_is_eol: 1 when the backend says the system version is end of life, else 0. */
  isEol(family: number, major: number, minor: number): number;
  eolWarning(): string;
  /** The ProbeResult JSON line; `rssKb` < 0 means not measured; throws if the backend refuses. */
  probe(args: ProbeArgs): string;
  /** What the project probe modes need of the backend (spec 002). */
  readonly project: ProjectApi;
}

/**
 * The project API of the backend (contracts/c-api-project.md) for the project probe modes of spec 002,
 * only what they need. A project or a report is a pointer (a number the page never interprets); 0
 * means none. The file lives in a buffer of the module (loomora_web_alloc, bridge/wasm/module.cpp)
 * until the project is closed.
 */
export interface ProjectApi {
  /** Copies `bytes` into a new buffer of the module; 0 when memory is short. */
  alloc(bytes: Uint8Array): number;
  release(pointer: number): void;
  /** loomora_project_open_memory on a buffer made by alloc. */
  open(pointer: number, length: number): { rc: number; project: number; report: number };
  /** loomora_validate_memory. */
  validate(pointer: number, length: number): { rc: number; report: number };
  /** loomora_project_is_light: 1 for a light project, 0 for a full one. */
  isLight(project: number): number;
  /** loomora_project_export_stream of every result into a buffer of the module (release it with release). */
  exportCopy(project: number, profile: number, modifiedAt: string): { rc: number; data: number; size: number };
  reportCount(report: number): number;
  reportErrorCount(report: number): number;
  reportFree(report: number): void;
  close(project: number): void;
}

export interface ProbeArgs {
  platformId: string;
  uiVariant: number;
  coreKind: number;
  input: string;
  displayedText: string;
  displayed: number;
  displayMs: number;
  rssKb: number;
  eolWarningShown: number;
  timestampUtc: string;
}

const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8");

/** Loads a classic script and resolves when it has run. */
function loadScript(src: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const script = document.createElement("script");
    script.src = src;
    script.onload = () => resolve();
    script.onerror = () => reject(new Error("cannot load " + src));
    document.head.appendChild(script);
  });
}

/** The WebAssembly instance of `url`: streaming compilation when the browser and the server allow
 * it, else the bytes as an ArrayBuffer. */
async function instantiate(
  url: string,
  imports: WebAssembly.Imports,
): Promise<WebAssembly.WebAssemblyInstantiatedSource> {
  if (typeof WebAssembly.instantiateStreaming === "function") {
    try {
      return await WebAssembly.instantiateStreaming(fetch(url), imports);
    } catch (error) {
      /* a wrong MIME type (a server that does not know application/wasm) is the usual cause: read
         the bytes instead; any other failure fails the same way again below */
      console.log("instantiateStreaming failed (" + String(error) + "), using the ArrayBuffer form");
    }
  }
  const response = await fetch(url);
  if (!response.ok) {
    throw new Error("cannot load " + url + ": HTTP " + response.status);
  }
  return WebAssembly.instantiate(await response.arrayBuffer(), imports);
}

/** Starts the backend. `scriptUrl` is the Emscripten glue, `wasmUrl` the module. */
export async function loadNative(scriptUrl = "loomora.js", wasmUrl = "loomora.wasm"): Promise<Native> {
  await loadScript(scriptUrl);
  const create = (globalThis as { createLoomora?: CreateModule }).createLoomora;
  if (typeof create !== "function") {
    throw new Error("the backend script did not define createLoomora");
  }
  /* The glue has no failure callback for instantiateWasm: a failed load would leave its promise
     pending for ever, so it is raced against one that rejects. */
  let rejectLoad: (reason: unknown) => void = () => undefined;
  const failed = new Promise<never>((_, reject) => {
    rejectLoad = reject;
  });
  const started = create({
    instantiateWasm(imports, done) {
      instantiate(wasmUrl, imports).then((result) => done(result.instance, result.module), rejectLoad);
      return {};
    },
  });
  return bind(await Promise.race([started, failed]));
}

function bind(M: EmscriptenModule): Native {
  const abi = M.ccall("loomora_abi_version", "number", [], []);
  if (abi !== ABI_VERSION) {
    throw new Error("the backend has ABI version " + abi + ", this page needs " + ABI_VERSION);
  }
  const n = "number";
  const greetFn = M.cwrap("loomora_greet", n, [n, n, n, n]);
  const isEolFn = M.cwrap("loomora_is_eol", n, [n, n, n]);
  const probeFn = M.cwrap("loomora_probe_record", n, [n, n, n, n, n, n, n, n, n, n, n, n, n]);

  /** A NUL-terminated copy of `text` on the stack of the module; returns its address. */
  function cString(text: string): number {
    const bytes = encoder.encode(text);
    const pointer = M.stackAlloc(bytes.length + 1);
    M.HEAPU8.set(bytes, pointer);
    M.HEAPU8[pointer + bytes.length] = 0;
    return pointer;
  }

  /** `count` bytes of the stack of the module set to 0 (the out-pointers of a call); returns the address. */
  function slots(count: number): number {
    const pointer = M.stackAlloc(count);
    M.HEAPU8.fill(0, pointer, pointer + count);
    return pointer;
  }

  /** A 32-bit little-endian value of the module's memory (a pointer or a size_t, 32 bits in WebAssembly). */
  function u32(pointer: number): number {
    return new DataView(M.HEAPU8.buffer).getUint32(pointer, true);
  }

  const project: ProjectApi = {
    alloc(bytes) {
      const pointer = M.ccall("loomora_web_alloc", n, [n], [bytes.length]);
      if (pointer !== 0) {
        M.HEAPU8.set(bytes, pointer);
      }
      return pointer;
    },

    release(pointer) {
      if (pointer !== 0) {
        M.ccall("loomora_web_free", null, [n], [pointer]);
      }
    },

    open(pointer, length) {
      const sp = M.stackSave();
      try {
        const s = slots(8);
        const rc = M.ccall("loomora_project_open_memory", n, [n, n, n, n, n, n], [pointer, length, 0, 0, s, s + 4]);
        return { rc, project: u32(s), report: u32(s + 4) };
      } finally {
        M.stackRestore(sp);
      }
    },

    validate(pointer, length) {
      const sp = M.stackSave();
      try {
        const s = slots(4);
        const rc = M.ccall("loomora_validate_memory", n, [n, n, n, n, n], [pointer, length, 0, 0, s]);
        return { rc, report: u32(s) };
      } finally {
        M.stackRestore(sp);
      }
    },

    isLight(handle) {
      return M.ccall("loomora_project_is_light", n, [n], [handle]);
    },

    exportCopy(handle, profile, modifiedAt) {
      const sp = M.stackSave();
      try {
        const s = slots(8);
        const rc = M.ccall("loomora_web_export", n, [n, n, n, n, n], [handle, profile, cString(modifiedAt), s, s + 4]);
        return { rc, data: u32(s), size: u32(s + 4) };
      } finally {
        M.stackRestore(sp);
      }
    },

    /* The counts are int64: a BigInt in the WebAssembly build. */
    reportCount(report) {
      return report === 0 ? 0 : Number(M.ccall("loomora_report_count", n, [n], [report]));
    },

    reportErrorCount(report) {
      return report === 0 ? 0 : Number(M.ccall("loomora_report_error_count", n, [n], [report]));
    },

    reportFree(report) {
      if (report !== 0) {
        M.ccall("loomora_report_free", null, [n], [report]);
      }
    },

    close(handle) {
      if (handle !== 0) {
        M.ccall("loomora_project_close", null, [n], [handle]);
      }
    },
  };

  return {
    project,

    greet(text) {
      const bytes = encoder.encode(text);
      const sp = M.stackSave();
      try {
        const inPtr = M.stackAlloc(bytes.length + 1);
        const outPtr = M.stackAlloc(GREET_CAP);
        M.HEAPU8.set(bytes, inPtr);
        const length = greetFn(inPtr, bytes.length, outPtr, GREET_CAP);
        if (length < 0) {
          throw new Error("loomora_greet failed with status " + length);
        }
        /* HEAPU8 is read again: the heap may have moved while the backend ran */
        return decoder.decode(M.HEAPU8.subarray(outPtr, outPtr + length));
      } finally {
        M.stackRestore(sp);
      }
    },

    isEol(family, major, minor) {
      return isEolFn(family, major, minor);
    },

    eolWarning() {
      return M.UTF8ToString(M.ccall("loomora_eol_warning", n, [], []));
    },

    probe(p) {
      const sp = M.stackSave();
      try {
        let capacity = PROBE_CAP;
        for (;;) {
          const buf = M.stackAlloc(capacity);
          const needed = probeFn(
            cString(p.platformId), p.uiVariant, p.coreKind, cString(p.input), cString(p.displayedText),
            p.displayed, p.displayMs, BigInt(p.rssKb), p.eolWarningShown, cString(""),
            cString(p.timestampUtc), buf, capacity,
          );
          if (needed === 0) {
            throw new Error("loomora_probe_record returned nothing");
          }
          if (needed <= capacity) {
            return decoder.decode(M.HEAPU8.subarray(buf, buf + needed));
          }
          capacity = needed; /* the return value is the length needed: ask again with room */
        }
      } finally {
        M.stackRestore(sp);
      }
    },
  };
}
