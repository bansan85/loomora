// The C API of the C++11 backend (core/include/loomora.h) as seen from Swift, through the module
// CLoomora. This file only converts between String (UTF-8 on the native side) and the C buffers;
// it builds no greeting itself.
import Foundation
import CLoomora

enum Native {
    static let expectedAbiVersion: UInt32 = 1 // LOOMORA_ABI_VERSION

    // loomora_* family and variant constants of loomora.h
    static let familyIos: Int32 = LOOMORA_FAMILY_IOS
    static let uiModern: Int32 = LOOMORA_UI_MODERN
    static let coreNative: Int32 = LOOMORA_CORE_NATIVE

    static func abiVersion() -> UInt32 {
        return loomora_abi_version()
    }

    // "Bonjour " + input, computed by the backend; nil when the backend reports an error.
    static func greet(_ input: String) -> String? {
        let length = input.utf8.count
        var output = [UInt8](repeating: 0, count: length + 64) // "Bonjour " + input
        let capacity = output.count
        let written = output.withUnsafeMutableBufferPointer { buffer in
            loomora_greet(input, length, buffer.baseAddress, capacity)
        }
        if written < 0 {
            return nil
        }
        return String(decoding: output[0..<Int(written)], as: UTF8.self)
    }

    static func isEol(family: Int32, major: Int32, minor: Int32) -> Bool {
        return loomora_is_eol(family, major, minor) == 1
    }

    // The text is static and owned by the backend: it is read, never freed.
    static func eolWarning() -> String {
        return String(cString: loomora_eol_warning())
    }

    static func buildCompiler() -> String {
        return String(cString: loomora_build_compiler())
    }

    // The ProbeResult JSON line (ASCII, without a line end), or nil if the backend cannot build it.
    static func probeRecord(platformId: String, input: String, displayedText: String,
                            displayed: Bool, displayMs: UInt32, rssKb: Int64,
                            eolWarningShown: Bool, disabledFeatures: String,
                            timestampUtc: String) -> String? {
        // First call with no room measures, second one fills the buffer.
        let needed = loomora_probe_record(platformId, uiModern, coreNative, input, displayedText,
                                          displayed ? 1 : 0, displayMs, rssKb,
                                          eolWarningShown ? 1 : 0, disabledFeatures, timestampUtc,
                                          nil, 0)
        if needed == 0 {
            return nil
        }
        var line = [UInt8](repeating: 0, count: needed)
        let written = line.withUnsafeMutableBufferPointer { buffer in
            loomora_probe_record(platformId, uiModern, coreNative, input, displayedText,
                                 displayed ? 1 : 0, displayMs, rssKb,
                                 eolWarningShown ? 1 : 0, disabledFeatures, timestampUtc,
                                 buffer.baseAddress, needed)
        }
        if written != needed {
            return nil
        }
        return String(decoding: line, as: UTF8.self)
    }
}

// ---- Project probe modes (docs/implementation/specs/002-loomora-file-format/contracts/ui-validate-project.md) ----
// --validate-project / --roundtrip-project (macOS) and -validate_project / -roundtrip_project (iOS).
// No application logic: the file is opened, validated and (round trip) exported and validated again
// by the backend through the C API; this only builds the one result line from the counts the report
// gives. The line is the summary line of `loomora validate --quiet`, byte for byte.

enum ProjectProbeMode {
    case validate
    case roundtrip
}

// Receives the chunks of the export (`user` of the sink callback).
private final class ExportBuffer {
    var bytes = [UInt8]()
}

extension Native {
    // The timestamp of the copy: the round trip checks the structure of the copy, not a date, and the
    // UI reads no clock and parses no part of the project (FR-027). One fixed valid value.
    private static let copyModifiedAt = "1970-01-01T00:00:00Z"

    private static func plural(_ n: Int64, _ noun: String) -> String {
        return "\(n) \(noun)" + (n == 1 ? "" : "s")
    }

    // A failure that leaves nothing to read (I/O, memory, bad argument): exit status 2, as the command.
    private static func unreadable(_ rc: Int32) -> Bool {
        return rc == Int32(LOOMORA_ERR_IO) || rc == Int32(LOOMORA_ERR_NO_MEMORY)
            || rc == Int32(LOOMORA_ERR_INVALID) || rc == Int32(LOOMORA_ERR_INTERNAL)
            || rc == Int32(LOOMORA_ERR_TOO_LARGE_FOR_PLATFORM)
    }

    // The summary line of `loomora validate --quiet` for a file: "OK <file>: 0 errors, 0 warnings",
    // "FAIL <file>: N errors, M warnings" or "FAIL <file>: error <code>" (the failure left no error in
    // the report); status is 0, 1 or 2 as the command's exit status. The counts come from the report.
    private static func summaryLine(name: String, rc: Int32,
                                    report: OpaquePointer?) -> (line: String, status: Int32) {
        let total: Int64 = report != nil ? Int64(loomora_report_count(report)) : 0
        let errors: Int64 = report != nil ? Int64(loomora_report_error_count(report)) : 0
        let warnings = total - errors
        let codeOnly = rc != 0 && errors <= 0
        let status: Int32
        if rc != 0 && unreadable(rc) {
            status = 2
        } else if rc != 0 || errors > 0 {
            status = 1
        } else {
            status = 0
        }
        let detail = codeOnly ? "error \(rc)" : plural(errors, "error") + ", " + plural(warnings, "warning")
        return ((status == 0 ? "OK " : "FAIL ") + name + ": " + detail, status)
    }

    // Runs `mode` on the file `path` (as it was given). Returns the line to print and the exit
    // status of the program: 0 valid (warnings allowed), 1 an error or a failed round trip, 2 the
    // file cannot be read at all.
    static func projectProbe(mode: ProjectProbeMode, path: String) -> (line: String, status: Int32) {
        var project: OpaquePointer?
        var report: OpaquePointer?
        let rc = loomora_project_open_path(path, nil, nil, &project, &report)
        let opened = summaryLine(name: path, rc: rc, report: report)
        if report != nil {
            loomora_report_free(report)
        }
        if rc != 0 || mode == .validate {
            if project != nil {
                loomora_project_close(project)
            }
            return opened
        }
        let result = roundTrip(name: path, project: project)
        loomora_project_close(project)
        return result
    }

    // A full copy of every result into memory, validated again.
    private static func roundTrip(name: String, project: OpaquePointer?) -> (line: String, status: Int32) {
        let copy = ExportBuffer()
        let profile = loomora_project_is_light(project) == 1 ? Int32(LOOMORA_PROFILE_LIGHT)
                                                           : Int32(LOOMORA_PROFILE_FULL)
        let sink: @convention(c) (UnsafeMutableRawPointer?, UnsafePointer<UInt8>?, Int) -> Int32 = {
            user, data, length in
            guard let user = user else {
                return 1
            }
            if let data = data, length > 0 {
                let buffer = Unmanaged<ExportBuffer>.fromOpaque(user).takeUnretainedValue()
                buffer.bytes.append(contentsOf: UnsafeBufferPointer(start: data, count: length))
            }
            return 0
        }
        var exportReport: OpaquePointer?
        let user = Unmanaged.passUnretained(copy).toOpaque()
        let exported = loomora_project_export_stream(project, profile, nil, 0, copyModifiedAt, 0,
                                                     sink, user, &exportReport)
        if exportReport != nil {
            loomora_report_free(exportReport)
        }
        if exported != 0 {
            return ("roundtrip FAIL export error \(exported)", 1)
        }
        var copyReport: OpaquePointer?
        let validated: Int32 = copy.bytes.withUnsafeBufferPointer { buffer in
            return loomora_validate_memory(buffer.baseAddress, buffer.count, nil, nil, &copyReport)
        }
        let summary = summaryLine(name: name, rc: validated, report: copyReport)
        if copyReport != nil {
            loomora_report_free(copyReport)
        }
        let line = (summary.status == 0 ? "roundtrip ok " : "roundtrip FAIL ") + summary.line
        return (line, summary.status == 0 ? 0 : 1)
    }
}
