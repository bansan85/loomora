// Probe mode (ui-contract.md): --probe, --probe-input <utf8>, and the project modes
// --validate-project <path> and --roundtrip-project <path> (Native.projectProbe). This file only
// reads the command line, measures (launch to first display, resident size) and reads the system; the JSON line
// itself comes from loomora_probe_record() through Native.probeRecord.
import AppKit
import Foundation

struct ProbeOptions {
    var enabled = false
    var input: String?
    var projectMode: ProjectProbeMode?
    var projectPath: String?
    var badUsage = false

    // Reads the raw argv: CommandLine.arguments silently repairs invalid UTF-8, the raw bytes
    // let the program refuse it, as the other UIs do.
    static func parse() -> ProbeOptions {
        var options = ProbeOptions()
        let argc = Int(CommandLine.argc)
        var index = 1
        var projectOptions = 0 // --validate-project and --roundtrip-project given, with their repeats
        while index < argc {
            let argument = rawArgument(index)
            if argument == "--probe" {
                options.enabled = true
            } else if argument == "--validate-project" || argument == "--roundtrip-project" {
                projectOptions += 1
                options.projectMode = argument == "--validate-project" ? .validate : .roundtrip
                index += 1
                if index < argc, let value = rawArgument(index) {
                    options.projectPath = value
                } else {
                    options.badUsage = true
                }
            } else if argument == "--probe-input" {
                index += 1
                if index < argc, let value = rawArgument(index) {
                    options.input = value
                } else {
                    options.badUsage = true
                }
            }
            index += 1
        }
        // The input override is only meaningful in probe mode.
        if options.input != nil && !options.enabled {
            options.badUsage = true
        }
        // The three modes exclude each other.
        if projectOptions > 1 || (projectOptions == 1 && (options.enabled || options.input != nil)) {
            options.badUsage = true
        }
        return options
    }

    // nil when the argument is missing or is not valid UTF-8.
    private static func rawArgument(_ index: Int) -> String? {
        guard let pointer = CommandLine.unsafeArgv[index] else {
            return nil
        }
        return String(validatingUTF8: pointer)
    }
}

enum Probe {
    private static let exitDelaySeconds = 2.0
    private static let launch = DispatchTime.now()
    private static var displayedAt: DispatchTime?
    private static var finishScheduled = false

    static let options = ProbeOptions.parse()

    // Call first thing in the app's init: records the launch time.
    static func start() {
        _ = launch
        _ = options
    }

    // Called each time the window content appears; only the first call counts as the first
    // display. In probe mode the first call also starts the 2-second countdown.
    static func windowAppeared(_ state: AppState) {
        if displayedAt == nil {
            displayedAt = DispatchTime.now()
        }
        if !options.enabled || finishScheduled {
            return
        }
        finishScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + exitDelaySeconds) {
            Probe.finish(state)
        }
    }

    // Project probe mode: the line of `loomora validate --quiet` goes to stdout; returns the exit status.
    static func runProject(_ mode: ProjectProbeMode, path: String) -> Int32 {
        let result = Native.projectProbe(mode: mode, path: path)
        FileHandle.standardOutput.write(Data((result.line + "\n").utf8))
        return result.status
    }

    static func writeError(_ message: String) {
        FileHandle.standardError.write(Data((message + "\n").utf8))
    }

    private static func displayMilliseconds() -> UInt32 {
        guard let shown = displayedAt, shown.uptimeNanoseconds >= launch.uptimeNanoseconds else {
            return 0
        }
        return UInt32(clamping: (shown.uptimeNanoseconds - launch.uptimeNanoseconds) / 1_000_000)
    }

    // After 2 seconds: the probe line goes to stdout and the application terminates (status 0).
    private static func finish(_ state: AppState) {
        let line = Native.probeRecord(platformId: SystemInfo.platformId,
                                      input: state.input,
                                      displayedText: state.greetingText,
                                      displayed: displayedAt != nil,
                                      displayMs: displayMilliseconds(),
                                      rssKb: SystemInfo.residentSizeKb(),
                                      eolWarningShown: state.eolWarningVisible,
                                      timestampUtc: SystemInfo.timestampUtc())
        guard let record = line else {
            writeError("loomora: the backend could not build the probe record")
            exit(1)
        }
        FileHandle.standardOutput.write(Data((record + "\n").utf8))
        NSApplication.shared.terminate(nil)
    }
}

// What the UI reads from the system: version (for the end-of-life test), architecture, memory,
// clock.
enum SystemInfo {
    // Real version only when the app is built with the macOS 11 SDK or later (it is): older SDKs
    // are told 10.16.
    static var version: (major: Int32, minor: Int32) {
        let v = ProcessInfo.processInfo.operatingSystemVersion
        return (Int32(v.majorVersion), Int32(v.minorVersion))
    }

    // Architecture of the machine, not of the slice: an x86_64 slice under Rosetta reports arm64.
    static var architecture: String {
        var isArm64: Int32 = 0
        var size = MemoryLayout<Int32>.size
        if sysctlbyname("hw.optional.arm64", &isArm64, &size, nil, 0) == 0 && isArm64 == 1 {
            return "arm64"
        }
        return "x86_64"
    }

    // "macos-<version>-<arch>", for example macos-26-arm64; matches ^[a-z0-9][a-z0-9._-]*$.
    static var platformId: String {
        let v = version
        let name = v.major >= 11 ? "\(v.major)" : "\(v.major).\(v.minor)"
        return "macos-\(name)-\(architecture)"
    }

    // Resident size of the process in kB from task_info, or -1 when it cannot be read.
    static func residentSizeKb() -> Int64 {
        var info = mach_task_basic_info()
        var count = mach_msg_type_number_t(MemoryLayout<mach_task_basic_info>.size
                                           / MemoryLayout<natural_t>.size)
        let capacity = Int(count)
        let status: kern_return_t = withUnsafeMutablePointer(to: &info) { pointer in
            return pointer.withMemoryRebound(to: integer_t.self, capacity: capacity) { raw in
                return task_info(mach_task_self_, task_flavor_t(MACH_TASK_BASIC_INFO), raw, &count)
            }
        }
        if status != KERN_SUCCESS {
            return -1
        }
        return Int64(info.resident_size / 1024)
    }

    // "YYYY-MM-DDTHH:MM:SSZ", the current time in UTC.
    static func timestampUtc() -> String {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime]
        formatter.timeZone = TimeZone(secondsFromGMT: 0)
        return formatter.string(from: Date())
    }
}
