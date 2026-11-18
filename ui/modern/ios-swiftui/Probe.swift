// Probe mode of the iOS Modern UI (ui-contract.md): launch arguments -probe and
// -probe_input <utf8>, and the project modes -validate_project <path> and -roundtrip_project <path>
// (Native.projectProbe, see Probe.projectOptions). The probe line is built by the backend (loomora_probe_record); this file
// only measures, converts and writes it, through NSLog. The app exits 2 seconds after the first
// display.
import Foundation

enum Probe {
    static let exitDelaySeconds = 2.0
    static let maxInputScalars = 1000

    private static var launchDate = Date()
    private static var displayedDate: Date?
    private static var started = false

    static let enabled: Bool = ProcessInfo.processInfo.arguments.contains("-probe")

    // The text given with -probe_input, only honoured in probe mode. The argument list is read
    // directly: UserDefaults would parse a value such as "(a, b)" as a property list.
    static let inputOverride: String? = {
        if !enabled {
            return nil
        }
        let arguments = ProcessInfo.processInfo.arguments
        if let index = arguments.firstIndex(of: "-probe_input") {
            return index + 1 < arguments.count ? arguments[index + 1] : ""
        }
        return UserDefaults.standard.string(forKey: "probe_input")
    }()

    // The project mode given as launch argument and its path. The argument list is read directly
    // (as for -probe_input). badUsage: both project modes, a project mode with -probe, or a project
    // mode without its path: the app then exits with status 2 and writes nothing.
    static let projectOptions: (mode: ProjectProbeMode?, path: String?, badUsage: Bool) = {
        let arguments = ProcessInfo.processInfo.arguments
        let validate = arguments.firstIndex(of: "-validate_project")
        let roundtrip = arguments.firstIndex(of: "-roundtrip_project")
        if validate == nil && roundtrip == nil {
            return (nil, nil, false)
        }
        let mode: ProjectProbeMode = validate != nil ? .validate : .roundtrip
        let index = validate ?? roundtrip!
        let path: String? = index + 1 < arguments.count ? arguments[index + 1] : nil
        let bad = (validate != nil && roundtrip != nil) || enabled || path == nil
        return (mode, path, bad)
    }()

    // Project probe mode: the line of `loomora validate --quiet` goes to NSLog; returns the exit status.
    static func runProject(_ mode: ProjectProbeMode, path: String) -> Int32 {
        if Native.abiVersion() != Native.expectedAbiVersion {
            NSLog("loomora: ABI version mismatch between the interface and the engine")
            return 1
        }
        let result = Native.projectProbe(mode: mode, path: path)
        NSLog("%@", result.line)
        return result.status
    }

    // The launch is the creation of the process, so the delay includes the start of the runtime and
    // not only the time spent in this code.
    static func start() {
        launchDate = processStartDate() ?? Date()
    }

    // At most maxInputScalars Unicode scalars (at most 4 bytes each: under the 4,096 bytes of the
    // backend), never cutting a scalar in two.
    static func limited(_ text: String) -> String {
        if text.unicodeScalars.count <= maxInputScalars {
            return text
        }
        var result = String.UnicodeScalarView()
        result.append(contentsOf: text.unicodeScalars.prefix(maxInputScalars))
        return String(result)
    }

    // Records the first display; further calls are ignored.
    static func markDisplayed() {
        if displayedDate == nil {
            displayedDate = Date()
        }
    }

    // Probe mode: once the screen is shown, wait 2 seconds, emit the line built from what the
    // screen holds then (`snapshot`), and exit.
    static func begin(snapshot: @escaping () -> (input: String, message: String, eolShown: Bool)) {
        markDisplayed()
        if !enabled || started {
            return
        }
        started = true
        DispatchQueue.main.asyncAfter(deadline: .now() + exitDelaySeconds) {
            let state = snapshot()
            let status = emit(input: state.input, message: state.message, eolShown: state.eolShown)
            exit(status)
        }
    }

    // Writes the probe line with NSLog; returns the exit status.
    static func emit(input: String, message: String, eolShown: Bool) -> Int32 {
        let line = Native.probeRecord(platformId: platformId(), input: input, displayedText: message,
                                      displayed: displayedDate != nil, displayMs: elapsedMs(),
                                      rssKb: residentKb(), eolWarningShown: eolShown,
                                      disabledFeatures: "", timestampUtc: timestampUtc())
        guard let record = line else {
            NSLog("loomora: the backend could not build the probe record")
            return 1
        }
        NSLog("%@", record)
        return 0
    }

    // "ios-<major>.<minor>-<arch>", with "-simulator" on the simulator.
    static func platformId() -> String {
        let version = ProcessInfo.processInfo.operatingSystemVersion
        var id = "ios-\(version.majorVersion).\(version.minorVersion)"
        #if arch(arm64)
        id += "-arm64"
        #elseif arch(x86_64)
        id += "-x86_64"
        #else
        id += "-unknown"
        #endif
        #if targetEnvironment(simulator)
        id += "-simulator"
        #endif
        return id
    }

    private static func elapsedMs() -> UInt32 {
        guard let shown = displayedDate else {
            return 0
        }
        let ms = shown.timeIntervalSince(launchDate) * 1000.0
        return ms > 0 ? UInt32(ms) : 0
    }

    private static func timestampUtc() -> String {
        return ISO8601DateFormatter().string(from: Date())
    }

    // Start time of this process (sysctl), nil if it cannot be read.
    private static func processStartDate() -> Date? {
        var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()]
        var info = kinfo_proc()
        var size = MemoryLayout<kinfo_proc>.stride
        if sysctl(&mib, UInt32(mib.count), &info, &size, nil, 0) != 0 {
            return nil
        }
        let created = info.kp_proc.p_un.__p_starttime
        return Date(timeIntervalSince1970: Double(created.tv_sec) + Double(created.tv_usec) / 1_000_000.0)
    }

    // Resident size in kB (task_info), -1 if it cannot be read.
    private static func residentKb() -> Int64 {
        var info = mach_task_basic_info()
        var count = mach_msg_type_number_t(MemoryLayout<mach_task_basic_info>.size / MemoryLayout<natural_t>.size)
        let status = withUnsafeMutablePointer(to: &info) { pointer in
            pointer.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { raw in
                task_info(mach_task_self_, task_flavor_t(MACH_TASK_BASIC_INFO), raw, &count)
            }
        }
        if status != KERN_SUCCESS {
            return -1
        }
        return Int64(info.resident_size / 1024)
    }
}
