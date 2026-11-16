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
