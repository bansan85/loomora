# Data Model: Loomora probe

The entities of the spec are **data records versioned in the repository** (JSON/Markdown). No database. They live in the implementation documentation: the git submodule `docs/implementation/` (repository `../loomora-docs`), so the paths below start with it.

## Platform (`docs/implementation/validation/platforms/<id>.json`)

| Field | Type | Rule |
|---|---|---|
| `id` | string | unique, e.g. `win-2000-sp4-x86` |
| `family` | enum | `windows` `macos` `linux` `android` `ios` `web` |
| `os_name`, `os_version` | strings | exact version (build/SP included) |
| `arch` | enum | `x86` `x86_64` `arm` `arm64` `wasm` `js` |
| `release_date` | date | used to compute the age |
| `age_years` | number | computed at trial date |
| `is_floor` | boolean | true if it is the decided floor of the family |
| `in_scope` | boolean | false = older than the floor (out of scope, no verdict) |
| `environment` | enum | `physical` `vm` `emulator` `browser-archive` |
| `environment_detail` | string | hypervisor/device, image, tool version |
| `eol` | boolean | vendor end of life → warning mandatory |

## Deliverable (`docs/implementation/validation/deliverables/<platform-id>-<variant>.json`)

`platform_id`, `ui_variant` (`legacy` | `modern`), `cpp_std` (always `c++11` for the backend), `compiler` (id and exact version that built the backend, e.g. `gcc-8.3.0-mingw-w64`, `clang-17-osxcross`, `emscripten-1.39.20-fastcomp`), `target` (triple or ABI, e.g. `i686-w64-mingw32`, `armeabi`), `output` (`native` | `wasm` | `js`), `ui_language`, `ui_toolkit`, `ui_compiler` (the UI's own toolchain, e.g. `mingw-w64-gcc-8.3.0`), `artifact` (name, size in bytes), `shared_loc` (backend lines), `glue_loc` (JNI / Emscripten lines), `ui_loc` (lines of the native UI), `sharing_ratio` (0–1, computed), `disabled_features[]`, `fallbacks[]` (Fallback ids used), `notes`.

## Fallback (`docs/implementation/validation/fallbacks.json`) — FR-021, FR-022

`id`, `description` (recent feature replaced), `macro` (feature macro, e.g. `LOOMORA_HAS_CXX17_NODISCARD`), `min_compiler` (version from which the recent feature is used), `platform_ids[]` (platforms taking the fallback path), `reason`.

## ProbeResult (see `contracts/probe-result.schema.json`)

Produced by the application itself in probe mode: `schema`, `platform_id`, `ui_variant`, `input`, `message`, `greeting_matches`, `core` (`cpp` | `cpp-wasm` | `cpp-js`), `compiler`, `displayed`, `display_ms`, `peak_rss_kb`, `eol_warning_shown`, `disabled_features`, `timestamp_utc`.

## Verdict (`docs/implementation/validation/verdicts/<platform-id>-<variant>.json`)

| Field | Rule |
|---|---|
| `platform_id`, `ui_variant` | reference to Platform (`in_scope = true` only) and its variant |
| `web_path` | `wasm` \| `js`; mandatory for the `web` family, forbidden otherwise |
| `status` | `ok` \| `ok_restricted` \| `ko` \| `not_tested` |
| `restrictions[]` | mandatory if `ok_restricted` |
| `disabled_features[]` | features disabled in a Legacy UI, with reason (FR-025) |
| `cause`, `attempts[]` | mandatory if `ko`; `reason` if `not_tested` |
| `evidence[]` | screenshots/recordings/logs (path + sha256); mandatory unless `not_tested` |
| `reproduction` | path of the procedure |
| `decision` | `keep` \| `restrict` \| `drop` |

**Transitions**: `not_tested` → (`ok` \| `ok_restricted` \| `ko`). Any `status` change between two runs is flagged (FR-018).

## Hypothesis (`docs/implementation/validation/hypotheses.json`)

Rows V1–V20 of the requirements document (§15.1.4): `id`, `title`, `criterion`, `fallback`, `covered_by_probe` (boolean), `platform_ids[]`, `status` (Verdict values or `not_covered`).

## Source (`docs/implementation/validation/sources.json`)

`id`, `url`, `title`, `accessed`, `claim`. Referenced by decisions (`docs/implementation/adr/`) and the report (FR-017).

## Spike (`docs/implementation/validation/spikes.json`)

`id` (S1–S10 of research.md, S4 removed), `question`, `status` (`open` \| `confirmed` \| `refuted`), `result`, `adr` (path of the decision it triggered, if any). A refuted spike must name the chosen fallback.

## Result records (`docs/implementation/validation/results/<spec>/`)

Free-form records of measurements and test runs made for one feature spec (for example `spec-002/`: cross-architecture hashes, Android and Web runs, SC-004 fuzzing, SC-005 timings, spike outputs), as Markdown or JSON. They are not verdicts and are not checked by `validate_records.py` beyond their existence; a verdict or an evidence `hashes.json` that cites one points to it by path. Review reports of the code (`validation/bug-reports/`, one Markdown file per review) are also free-form and not checked.

## Web: two paths

For the `web` family, each tested browser has **one verdict per path** (`wasm`, `js`); the oldest platform of each path is recorded (FR-023). Browsers are tested per variant like other platforms.

## Relationships

`Platform 1—n Verdict` (one per variant) ; `Platform 1—n Deliverable` (one per variant) ; `Deliverable n—n Fallback` ; `Platform 1—n ProbeResult` ; `Hypothesis n—n Platform` ; `Verdict n—n Source` ; `Spike 1—n Deliverable` (a spike gates the deliverables that depend on its toolchain).
