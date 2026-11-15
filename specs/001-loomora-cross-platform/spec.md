# Feature Specification: Loomora Across 15 Years of Platforms

**Feature Branch**: `001-loomora-cross-platform` *(no git repository: no branch created)*

**Created**: 2026-11-12

**Status**: Draft

**Input**: User description (translated from French): "I want software that runs on as many platforms as possible, up to 15 years old if possible. So I want to validate the technical specifications and build a small UI that displays the text "Coucou". The goal is to share as much code as possible, knowing it will most likely be very hard because 15 years of history must be covered. Of course, you can use the Internet to retrieve old information."

> This feature is **Phase 0** of the Loomora project (see [docs/requirements-specification.md](../../docs/requirements-specification.md), §15.1): a minimal piece of software (Loomora) is used as a probe to establish, with evidence, how far back each platform family can be supported.

## Definitions

- **15-year window**: systems (and hardware) released from **November 2011** onward (reference date: 2026-11-12 minus 15 years) that can still run today.
- **Platform**: an operating system + version + hardware architecture combination (for example "Windows XP SP3, x86").
- **Family**: Windows, macOS, Linux, Android, iOS/iPadOS, Web.
- **Loomora application**: the minimal software of this feature; it shows a text field (pre-filled with "Coucou") and, next to it, the result of the greeting function applied to the field content (so "Bonjour Coucou" at start). It does nothing else functional.
- **Backend**: the shared logic of the Loomora application (the greeting function, whether the system is end-of-life, the probe record). It exists once, in **C++11** (ISO C++ 2011, no later language standard), with no second implementation in any other language.
- **Greeting function**: the backend function that takes a string and returns `"Bonjour "` followed by that string; it is the shared-logic test of the probe, and the UI displays its result.
- **UI**: the thin native layer that creates the window or screen on a platform and displays what the backend provides. It contains no application logic.
- **Legacy UI**: a UI implementation compatible with very old systems; some features may be disabled (each one documented).
- **Modern UI**: a UI implementation using the most modern interface technology available on the platform, preferably native to it.
- **Variant**: one of the two UI implementations of a platform (Legacy or Modern). Each platform has both variants, built on the same backend.
- **Floor**: the minimum version of a family decided for this probe (see Clarifications). A system older than the floor is **out of scope** (not attempted, no KO verdict).
- **Platform verdict**: given per platform and per variant. **OK** (works, full probe capabilities), **OK with restrictions** (works, but a capability is degraded or disabled, documented), **KO** (does not work; cause documented), **not tested** (could not be tried; reason documented).

## Clarifications

### Session 2026-11-12

- Q: What are the minimum target systems? → A: Superseded by the second session below (backend moved from Rust to C++11, which lowers the floors).
- Q: May the shared software exist as two implementations? → A: No. A single implementation of the backend, with no second implementation in another language.
- Q: How should the Web be handled? → A: WebAssembly is **mandatory** for modern browsers. If a browser does not support it, the same C++11 code is compiled to JavaScript (Emscripten asm.js / `-s WASM=0` output; WebAssembly-free fallback, not a second implementation).
- Q: Does "100% shared language" include the interfaces (windows)? → A: No. The **backend** (shared logic) is 100% C++11; the **UI** is written in each platform's native language.
- Q: How should differences between recent and old systems be handled? → A: Use recent compiler and system features when available, with a fallback when the toolchain or the system is old; the backend source itself stays strictly C++11.
- Q: Should each platform have a single UI or two? → A: **Two UI implementations per platform** on one shared backend: a Legacy UI (compatible with very old systems, features possibly disabled) and a Modern UI (the most modern interface possible, preferably native to the platform). The "no double implementation" rule applies to the backend only.
- Q: In which language must the specification be written? → A: **English** for every file in the `specs/` folder.
- Q: How is the backend exercised by the probe? → A: The backend provides a **greeting function** that takes a string and returns `"Bonjour " + string`; the UI displays the **result of that function**.
- Q: What string does the UI pass to the greeting function? → A: The text **typed by the person** in a text field (pre-filled with "Coucou"); the UI displays `"Bonjour "` + that text.
- Q: Is the label prefix "Coucou " or "Bonjour "? → A: The UI passes the **content of the text field** to the backend function, which returns `"Bonjour "` (capital B) + that parameter; the label displays this result. The prefix lives only in the backend, never in a UI. The field starts pre-filled with "Coucou", so the label starts as "Bonjour Coucou".
- Q: What is the Windows Legacy UI? → A: Superseded by the third session below (MFC dropped). Any UI may use its platform's native language, including C++.
- Q: How is the Linux Modern UI delivered? → A: **No Flatpak.** A plain executable that can be built on any recent distribution having the required libraries.
- Q: What is the name of the project and of this probe? → A: **Loomora**. Everything is renamed accordingly: the feature directory (`001-loomora-cross-platform`), the application, the C++ namespace `loomora`, the C API prefix `loomora_`, the header `loomora.h`, the libraries and the Java package `fr.bansan.loomora.probe`. "Coucou" stays only as the default text of the input field and in the greeting `Bonjour Coucou`.
- Q: How is SC-011 (reference inputs on every UI) tested? → A: Every UI accepts a probe input override so a runner can feed each reference input and compare the displayed greeting with `"Bonjour "` + input.

### Session 2026-11-12 (second, backend language change)

- Q: Which language holds the backend? → A: **C++11**, not Rust. Rust was dropped because it is too complicated for supporting old platforms. All previously existing project files (code, crates, build files) were deleted by the project owner; only the documents under `specs/` and `docs/` remain, and the plan, research, data model, contracts and tasks written for the Rust backend are obsolete and must be regenerated.
- Q: Without Rust, what is the oldest system targeted for each family? → A: Go down as far as C++11 allows. Floors: **Windows 2000**; **macOS 10.6 Snow Leopard** (64-bit; older versions only as a documented best effort); **Android 2.3 (API 9)**; **iOS 5** (iOS 4 and older out of scope); **Linux: any Linux distribution on which a C++11 compiler, CMake and the libX11 development files can be installed (binaries are built on the target machine, ADR 0007; a very old distribution may need a newer compiler installed)** (it was "kernel 2.6 with glibc 2.12" until 2026-11-14, when the owner decided that Linux binaries are always built on the machine that runs them). Systems older than these floors are out of scope. Many floors now predate the 15-year window (November 2011); the window stays the goal, the floors are how far the trials may go.
- Q: Which CPU architectures must be built and tested per platform? → A: The architectures the systems actually used: x86 32-bit and x64 (Windows, Linux, macOS 10.6); ARMv6/ARMv7 and x86 (Android 2.3); ARMv7 and arm64 (iOS), with ARM64 added on current systems. Other architectures (PowerPC, MIPS, Itanium, etc.) are out of scope.
- Q: What is the oldest web browser targeted for the JavaScript build? → A: The browsers of November 2011 (IE 9, Firefox 8, Chrome 15, Safari 5.1, Opera 11.5) are the target floor. Older browsers (IE 6 to 8, etc.) are attempted best effort and recorded, never blocking.

### Session 2026-11-12 (third, UI toolkit decisions)

- Q: If MFC cannot run on Windows 2000, is a Win32 C++ interface acceptable for that version? → A: MFC is **dropped** for the Windows Legacy UI on every Windows version. The Windows Legacy UI is written in **C++ on the plain Win32 API** (no MFC), one source for Windows 2000 → 11, built with the same C++11 compiler as the backend. The Modern UI (WinUI 3) is unchanged. This deviates from the requirements document (§2.1.2, MFC) and the deviation is listed in the report.
- Q: How is the WinUI 3 Modern UI connected to the C++11 backend? → A: Written in C#, calling the C API of the backend directly (`LibraryImport`); **no C++/CLI**.
- Q: Which toolkit and language for the Linux Modern UI? → A: **GTK 3 with gtkmm in C++, built with CMake** (owner decision 2026-11-14, replacing the first answer of 2026-11-12, "GTK 3, written in C" built with Meson; no libadwaita), as a plain executable without Flatpak, using only the GTK 3.10 API subset so it runs on distributions from about 2014 (floor to be confirmed by a trial). The UI uses the system gtkmm packages (gtkmm-3.0) and may be built against GTK 4 with gtkmm-4.0 through a CMake option; the C++ standards are C++11 for the Legacy UIs and up to C++17 for the Modern UIs (this one: C++11 with gtkmm-3.0, C++17 with gtkmm-4.0), while the backend stays C++11 whatever the UI uses (ADR 0006). The Linux Legacy UI (Xlib) is reserved for systems older than that, so a recent distribution such as Ubuntu 22.04 never needs the Legacy UI.

### Session 2026-11-15 (fourth, PoC validation scope)

- Q: On how many systems and processors must each UI be validated? → A: This is a proof of concept, so validating every system version and architecture is out of scope. The **Legacy UI** must run correctly on the **lowest testable system** of its family (Windows 2000, Android API 16, manylinux2010 for Linux); the **Modern UI** on **one system** (Windows 11, Android API 24, Ubuntu 24.04 under WSL) and on **one 64-bit processor**. Success on one system and one architecture is taken as success on every other system and architecture of the range; a failure elsewhere would be a detail to fix later. Rows not run (other Windows, Android and Linux versions, ARM, i686) stay `not_tested` in the records and are not failures. Consequences: SC-002 and SC-009 below are read with this rule, Windows XP is concluded by inference from Windows 2000 (no XP system is run), and the version ladders of User Story 2 (T088, T091, T092) stop at the lowest testable system.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - See the greeting on current platforms with the Modern UI (Priority: P1)

A person launches the Modern UI on a current system of each family (Windows 10/11 x64 and ARM64, macOS, Linux, Android, iPhone/iPad, web browser) and sees a text field containing "Coucou" and the greeting "Bonjour Coucou" displayed legibly in a window or screen that looks native to the platform. When they type another text (for example "Marie"), the greeting updates to "Bonjour Marie".

**Why this priority**: it is the reference baseline: nothing else is comparable without it. It proves the full chain (build, package, install, launch, display) on the tier A targets of the requirements document (§2.1.1).

**Independent Test**: install the Modern UI deliverable of a current platform on a test device, launch it, see "Bonjour Coucou", type a text, and see the greeting update. Testable platform by platform, independently of the others.

**Acceptance Scenarios**:

1. **Given** a current device of a supported family, **When** the person installs and launches the Modern UI, **Then** the window or screen shows the greeting "Bonjour Coucou" in under 3 seconds.
2. **Given** the application is displayed, **When** the person types "Marie" in the text field, **Then** the greeting shows "Bonjour Marie" within 1 second.
3. **Given** the application is displayed, **When** the person closes it, **Then** it ends cleanly with no error message and no leftover process.
4. **Given** a system set to dark theme or high contrast, **When** the application is launched, **Then** the text stays legible (sufficient contrast).

---

### User Story 2 - Push the compatibility floor back to the decided floors with the Legacy UI (Priority: P1)

For each family, the Legacy UI is run on older and older systems, down to the decided floors (Windows 2000, macOS 10.6, Linux with a C++11 compiler installable on it, Android 2.3, iOS 5, browsers of November 2011 with or without WebAssembly, older ones best effort), to determine for each whether the greeting is displayed and updates when text is typed. When an old system cannot be reached, the cause and any workaround are documented. Features disabled in the Legacy UI are listed.

**Why this priority**: it is the central requested goal: knowing how far back we can go, family by family, and with which technical solution.

**Independent Test**: pick a family, install an old system within its range (physical, virtual or emulated machine), launch the Legacy UI, and record the verdict. Each family is tested separately.

**Acceptance Scenarios**:

1. **Given** a system within a family's range and a Legacy UI deliverable built for it, **When** the person launches the application, **Then** the greeting "Bonjour Coucou" is displayed and the verdict is recorded (OK or OK with restrictions).
2. **Given** a system within range for which no solution succeeds, **When** the trials are over, **Then** the KO verdict is recorded with the precise cause, the solutions tried and their results.
3. **Given** an old system without modern Internet access (obsolete security protocols, expired certificates), **When** the person obtains the deliverable, **Then** an offline installation path (file, removable media) exists or its absence is documented.
4. **Given** an old platform past its vendor end of life, **When** the application is launched, **Then** a non-blocking warning tells the person that the system is no longer maintained (decision already taken for Windows XP, requirements document §2.1.2).
5. **Given** the Legacy UI runs with a feature disabled, **When** the verdict is recorded, **Then** the disabled feature and the reason are listed in the verdict.

---

### User Story 3 - Get a validation report of the technical specifications (Priority: P1)

The project owner receives a report that compares each technical choice of the requirements document (matrix V1 to V20, §15.1.4) with the old platforms actually tested: verdict per variant, measurements, fallback chosen, and recommendations to update version floors (for example the current macOS 11, iOS 15, Windows 10 of the requirements document), tiers A/B/C, and disabled features.

**Why this priority**: the user explicitly wants to "validate the technical specifications"; this is the go/no-go decision (milestone G0) on which the rest of the project depends.

**Independent Test**: read the report alone and check that, for each declared platform and variant, it contains verdict, evidence (screenshot or recording), exact system version, and how to reproduce.

**Acceptance Scenarios**:

1. **Given** the trials are finished, **When** the owner opens the report, **Then** it holds a platform × variant × verdict table covering 100% of the platforms targeted by the requirements document and those of the decided floors actually attempted.
2. **Given** a verdict OK with restrictions or KO, **When** the owner reads the matching row, **Then** they see the restriction or cause, the evidence, and the proposed decision (keep, restrict, drop the target).
3. **Given** a floor of the requirements document contradicted by the trials (an older version works, or a version announced does not), **When** the report is published, **Then** it lists the gap and the proposed change to the requirements document.

---

### User Story 4 - Share as much code as possible across platforms and variants (Priority: P2)

A developer changes the greeting (for example the prefix "Bonjour ") or the minimal behavior of the application in one place, and the change appears on every platform and in both variants without touching any platform-specific code. Platform-specific code is limited to what is strictly necessary (window, system integration), and the Legacy and Modern UIs of a platform share the same backend.

**Why this priority**: sharing as much code as possible is the underlying goal (long-term maintenance), but it only has value once the platforms are reachable (stories 1 to 3).

**Independent Test**: change the prefix "Bonjour " to "hello " in the single shared source, rebuild for all platforms and both variants, check that all show "hello Coucou" with no other change; measure the shared-code share.

**Acceptance Scenarios**:

1. **Given** the text is defined in one place, **When** it is changed, **Then** every built platform and variant shows the new text with no platform-specific code change.
2. **Given** the code of the whole project, **When** the shared-code share is measured, **Then** it is computed per platform and per variant and published in the report, with a justified list of every piece of platform-specific code.
3. **Given** a platform where sharing is impossible (age, missing tools), **When** a separate implementation is needed, **Then** it is isolated, minimal and justified in the report.

---

### User Story 5 - Replay the validation identically (Priority: P3)

Any team member can redo the trials on a platform (even an old one) from the documentation and the described environments, and get the same verdict, so that it serves later as a regression test.

**Why this priority**: the requirements document plans to keep the probe as a regression test for old platforms (§15.1.5); useful but later than the first validation.

**Independent Test**: a person who did not take part in the trials follows the procedure for an old platform and finds the recorded verdict.

**Acceptance Scenarios**:

1. **Given** the procedure of a platform, **When** another person follows it, **Then** they get the same verdict and the same display.
2. **Given** a new version of the probe, **When** the trials are replayed, **Then** any verdict change compared with the previous report is flagged.

---

### Edge Cases

- **Systems that no longer accept recent software**: executable blocked for lack of signature, up-to-date root certificate or modern security protocol. The deliverable must be installable offline, or the blockage is documented.
- **32-bit and ARM architectures** (32-bit x86, 32-bit ARM): the display must be identical; the absence of a deliverable is a verdict, not an oversight. Architectures outside the decided list (PowerPC, MIPS, Itanium) are out of scope.
- **Hardware without graphics acceleration** or with period drivers: display must work without 3D.
- **Old fonts and character sets**: if the system cannot display some characters, the prefix "Bonjour " and Latin text must stay legible; behavior on non-Latin text and emoji is observed and recorded, not blocking, and the application never crashes.
- **Very small or very large screens, high scaling** (2011 phone at 320×480, 4K display): the text must stay fully visible.
- **End-of-life system without security patches**: non-blocking warning; the application opens no network access.
- **Platform that cannot be tested** (no hardware, no system image, emulation impossible): verdict "not tested", distinct from KO, with the reason.
- **Both variants on one machine**: a modern system holds the Legacy and Modern UIs; they must not conflict (distinct names, no shared mutable state).
- **Modern UI toolkit missing on an in-range system**: the platform falls back to its Legacy UI and the gap is recorded.
- **Repeated or simultaneous launch**: two instances do not disturb each other.
- **Abrupt interruption** (process kill, power cut): normal relaunch, no corrupted state.
- **Empty text**: with an empty field the greeting is "Bonjour " (prefix only), with no error.
- **Very long text**: the field limits input to 1,000 characters; pasting more is truncated without error; the greeting stays scrollable or wrapped, never overflowing the window.
- **Special text**: accents, CJK, right-to-left text, emoji and combining characters are passed to the greeting function and returned unchanged after the prefix.
- **No keyboard** (probe mode, kiosk, some old touch devices): the field keeps its default "Coucou" and the greeting is still displayed.
- **System language**: the prefix "Bonjour " is always shown as is; a system language or writing direction (right to left) does not break the display.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The Loomora application MUST show a text field pre-filled with "Coucou" and MUST display, legibly, the result of the greeting function applied to the field content, in a window or screen native to the platform, updating on every change of the field.
- **FR-002**: The application MUST launch, display and close cleanly on every platform declared with verdict OK or OK with restrictions, in each variant.
- **FR-003**: The project MUST provide, for each family (Windows, macOS, Linux, Android, iOS/iPadOS, Web), an installable (or directly runnable) deliverable for both the Legacy UI and the Modern UI.
- **FR-004**: The project MUST attempt, for each family, the systems from the decided floor (see Clarifications) up to the most recent, and record a verdict per attempted platform and variant; systems older than the floor are out of scope.
- **FR-005**: For each family and each variant, the project MUST identify the **oldest platform** on which the greeting is actually displayed, with evidence (screenshot or recording and exact system version).
- **FR-006**: The project MUST record, for each platform: family, exact system and version, architecture, hardware or virtualization used, verdict per variant, restrictions, cause in case of KO, and reproduction procedure.
- **FR-007**: The project MUST confront each hypothesis of the requirements document's matrix V1–V20 (§15.1.4) with the results and give a verdict per hypothesis concerning the old platforms tested, or mark it "not covered by this probe".
- **FR-008**: The project MUST produce a **validation report** containing the platform × variant × verdict matrix, the measurements (deliverable size, display time, memory used when the platform allows measuring it, otherwise recorded as not measured with the reason) and recommendations to change the requirements document (version floors, tiers A/B/C, disabled features).
- **FR-009**: The greeting prefix and the behavior of the application MUST be defined in **one single shared place** used by all platforms and both variants; a change there MUST propagate to all of them with no platform-specific change.
- **FR-010**: Platform-specific UI code, in either variant, MUST be limited to system integration (window, lifecycle, packaging, encoding conversion) and MUST contain no application logic.
- **FR-011**: The project MUST measure and publish the shared-code share per platform and per variant, and justify each separate implementation.
- **FR-012**: When an old platform cannot receive the common deliverable, the project MUST document the workaround chosen (dedicated deliverable, degraded version, giving up) and its cost in non-shared code.
- **FR-013**: On operating systems no longer maintained by their vendor (web browsers are not covered), the application MUST show a non-blocking warning and MUST open no network access.
- **FR-014**: The application MUST stay legible with the system's light, dark and high-contrast themes when they exist.
- **FR-015**: The application MUST work without graphics acceleration, without an Internet connection and without administrator rights at run time.
- **FR-016**: The test environments (system images, emulators, machines, tool versions) MUST be described so that the validation can be replayed by another person.
- **FR-017**: The project MUST record every structuring technical decision (choice of a build tool or approach for a family) with its rejected alternatives and the reason, including the old information found on the Internet (sources cited).
- **FR-018**: The results MUST be usable as a regression baseline: a verdict change between two runs of the validation MUST be flagged.
- **FR-019**: The backend (shared logic) MUST exist as **one single implementation in C++11**; the backend source MUST NOT require any later C++ standard, and no second implementation in another language is allowed.
- **FR-020**: The UI of each platform MAY be written in that platform's native language (including C and C++, for example the plain Win32 API in C++ for the Windows Legacy UI), but MUST stay within the role defined by FR-010 and call the C++11 backend.
- **FR-021**: The project MUST use recent compiler and system features when available, with an explicit fallback when the toolchain or the system is old; each fallback MUST be listed in the report with the platform that requires it.
- **FR-022**: The backend MUST compile as C++11 with the oldest compiler and standard library imposed by a floor on each platform (the toolchain per platform is chosen at planning and recorded in the report), and any more recent feature MUST stay behind a fallback.
- **FR-023**: On the Web, the browser MUST run the C++11 backend as WebAssembly; if it does not support it, it MUST load the JavaScript build of the same C++ code (Emscripten, WebAssembly disabled) and show the same result. The JavaScript build MUST work on the November 2011 browsers (IE 9, Firefox 8, Chrome 15, Safari 5.1, Opera 11.5); older browsers are best effort. The oldest browser platform that works MUST be recorded for each of the two paths.
- **FR-024**: Each platform MUST have two UI implementations on the same backend: a **Legacy UI** (compatible with very old systems) and a **Modern UI** (the most modern interface possible, preferably native).
- **FR-025**: The Legacy UI MAY disable features that the old system cannot provide, but each disabled feature MUST be listed in the verdict with the reason; the greeting MUST still be displayed.
- **FR-026**: The Modern UI MUST use the most modern interface technology available on its platform, native to the platform whenever one exists; the choice and its minimum system version MUST be recorded per platform.
- **FR-027**: Both variants of a platform MUST show the same greeting for the same input, provided by the same backend, and MUST be tested against the same reference test cases.
- **FR-028**: The backend MUST provide a greeting function that takes a string and returns exactly `"Bonjour "` followed by that string, for any valid UTF-8 input (including the empty string) up to 4,096 bytes; larger inputs MUST be rejected with a defined error, never a crash.
- **FR-029**: Every UI, in both variants, MUST obtain each displayed greeting by calling the backend greeting function; no UI may build the greeting itself.
- **FR-030**: The repository MUST have a continuous integration on GitHub that builds every UI and runs the unit tests, the reference vectors and the validation checks on every platform and variant of the validation, with no manual step: GitHub-hosted runners for what they can run, and a self-hosted runner that drives virtual machines (VirtualBox) and containers (Docker) for the systems a hosted runner cannot provide. The images of the virtual machines and containers are supplied by the owner and are not stored in the repository.

### Key Entities

- **Platform**: system + version + architecture + test hardware or environment; carries its verdicts (one per variant) and its evidence.
- **Variant**: Legacy UI or Modern UI of a platform; has its own deliverable, toolchain and disabled features.
- **Verdict**: OK, OK with restrictions, KO or not tested; accompanied by the cause or restrictions, the disabled features and the proposed decision.
- **Technical hypothesis**: a row V1–V20 of the requirements document; linked to the platforms it concerns.
- **Deliverable**: the installable or runnable package of a platform variant; states the shared-code share and the platform-specific code.
- **Validation report**: the synthesis of verdicts, measurements, gaps with the requirements document and recommendations.
- **Documentary source**: reference (page, archive, version) justifying a piece of information about an old system.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: With the Modern UI, the greeting "Bonjour Coucou" is displayed in under 3 seconds after launch, and updates within 1 second after typing, on 100% of the tier A platforms of the requirements document (§2.1.1) that are at or above the Modern UI floor of their family; tier A platforms below that floor (for example Android API 21 to 23) are served by the Legacy UI and recorded as such.
- **SC-002**: For each of the 6 families, the lowest testable platform (see the Clarifications of 2026-11-15) is tried with the Legacy UI and the result is recorded with evidence, and at least **3 families out of 6** are validated with the Legacy UI on a system released **10 years ago or more**.
- **SC-003**: At least **1 family** is validated with the Legacy UI on a system released **15 years ago or more** (full window or older), or its impossibility is demonstrated for each family by a documented cause and at least 2 solutions tried.
- **SC-004**: 100% of the platforms and variants attempted have a verdict, evidence and reproduction procedure in the report; no platform is left without a verdict.
- **SC-005**: 100% of hypotheses V1–V20 concerning the old platforms tested receive a verdict, or the mention "not covered by this probe" with the reason.
- **SC-006**: A change of the greeting prefix, made in one single place, appears on 100% of the built platforms and on both variants with no other code change.
- **SC-007**: At least **80%** of the application code (excluding generated code and packaging) is shared across platforms, the exact share is published per platform and per variant, and any gap below this value is justified.
- **SC-008**: A person outside the trials reproduces the verdict of at least 3 old platforms from the documentation alone, with identical results.
- **SC-009**: The go / no-go decision on Windows 2000 and Windows XP is made (the report concludes OK, OK with restrictions or KO; XP is concluded by inference from Windows 2000 when no XP system is run, see the Clarifications of 2026-11-15), together with the possible revision of the current floors of the requirements document (Windows 10, macOS 11, iOS 15, Linux Ubuntu 20.04, Android API 21).
- **SC-010**: For 100% of the platforms attempted, both variants have been built and tested, or the absence of one variant is justified in the report.
- **SC-011**: For 100% of a fixed list of reference inputs (empty, ASCII, accented, CJK, right-to-left, emoji, 1,000 characters), the greeting displayed equals `"Bonjour "` + the input, character for character, on every tested platform and variant.

## Assumptions

- **Definition of 15 years**: we go back to November 2011 from today's date. The systems actually targeted are those of the decided floors (see Clarifications): Windows 2000 and later (dates from 2000, beyond the window); macOS 10.6 (2009); Android 2.3 (2010); iOS 5 (2011); Linux (any distribution on which a C++11 compiler can be installed, built on the machine itself); browsers with WebAssembly (about 2017 onward), plus a JavaScript fallback for older browsers whose exact floor is determined by the trials (goal: 2011). Systems older than these floors (macOS 10.5 and earlier, Android 2.2 and earlier, iOS 4 and earlier, Windows 98/ME, Linux 2.4) are out of scope.
- "If possible" means that failure on a platform is an **acceptable result** if documented; the goal is knowledge, not forced delivery.
- The application is deliberately minimal: one text field and one greeting, no trace computation, no network, no storage, no voice, no printing. Hypotheses of the requirements document that require those capabilities (V2–V5, V11–V18) are studied only to the extent the probe exercises them, otherwise marked "not covered by this probe" and handed to an extended probe.
- The requirements document ([docs/requirements-specification.md](../../docs/requirements-specification.md)) is authoritative for context (a shared core, now C++11 instead of Rust, native interfaces per platform, tiers A/B/C; its Windows legacy UI is MFC, replaced here by the plain Win32 API in C++, see Clarifications). This specification describes **what** to validate; the **how** (tools, libraries) belongs to planning and may challenge those choices if the trials require it.
- The Modern UI of each platform is chosen at planning among the interface technologies of the requirements document (§2.2.1: WinUI 3 on Windows, SwiftUI on Apple platforms, Kotlin / Jetpack Compose on Android, GTK 3 with gtkmm on Linux (changed from GTK 4, then from C to C++ with gtkmm, see Clarifications), a modern web UI on the Web). The minimum system version of each Modern UI is the one its technology allows and is recorded in the report; it is not a floor decided in advance.
- Preliminary research findings (to be verified during the trials, not contractual):
  - Qt 4.8 (2011) covered Windows XP/7, macOS 10.6 and Linux; Qt 5.15 covers Windows 7, Android API 21, iOS 12; Qt 6 requires Windows 10. Flutter is too recent for the window (Android API 24+, iOS 13+). Sources: [Qt 4.8](https://doc.qt.io/qt-4.8/supported-platforms.html), [Qt 5.15](https://doc.qt.io/qt-5.15/supported-platforms.html), [Qt 6](https://doc.qt.io/qt-6/supported-platforms.html), [Flutter](https://docs.flutter.cn/reference/supported-platforms.md).
  - Consequence: the current floors of the requirements document (macOS 11, iOS 15, Windows 10) are far from the 15-year window; reaching the decided floors requires a per-family toolchain able to build C++11 for the oldest system, hence strict C++11 backend code and a fallback to take advantage of recent features. The Modern UI builds may use a more recent toolchain.
- Old systems are tested on virtual machines, emulation or period hardware depending on availability; access to real old hardware is not guaranteed.
- The application collects and sends no data; no personal data is handled.
- Code signing and publication in application stores are out of scope for this probe (hypothesis V20 postponed), unless they block installation on a tested platform.
- Language: the greeting prefix is "Bonjour " (French) on all platforms, whatever the system language; all project specification documents (everything under `specs/`) are written in English.
