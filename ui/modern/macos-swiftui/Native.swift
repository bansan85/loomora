// Thin wrapper over the C API of the backend (core/include/loomora.h, imported through the
// CLoomora module map). Nothing here decides anything: it only converts Swift types to the
// buffers of the C API and back. No function of the API returns memory to free, and the strings
// it returns are static.
import CLoomora
import Foundation

struct GreetError: Error {
    let code: Int32
}

enum Native {
    // Largest greeting: 1,000 characters of at most 4 bytes each, plus "Bonjour ".
    private static let greetingCapacity = 4096 + 64

    static var abiVersion: UInt32 {
        return loomora_abi_version()
    }

    static var abiVersionMatches: Bool {
        return loomora_abi_version() == UInt32(LOOMORA_ABI_VERSION)
    }

    static var expectedAbiVersion: UInt32 {
        return UInt32(LOOMORA_ABI_VERSION)
    }

    // "Bonjour " + input, computed by the backend. The input goes in as UTF-8 bytes; the
    // terminating NUL of the C string is not counted, but it keeps the pointer valid for "".
    static func greet(_ input: String) -> Result<String, GreetError> {
        let bytes = input.utf8CString
        var out = [UInt8](repeating: 0, count: greetingCapacity)
        let outCapacity = out.count
        let written: Int32 = bytes.withUnsafeBufferPointer { inBuffer in
            return out.withUnsafeMutableBufferPointer { outBuffer in
                return loomora_greet(inBuffer.baseAddress, inBuffer.count - 1,
                                     outBuffer.baseAddress, outCapacity)
            }
        }
        if written < 0 {
            return .failure(GreetError(code: written))
        }
        return .success(String(decoding: out[0..<Int(written)], as: UTF8.self))
    }

    static func isEndOfLife(family: Int32, major: Int32, minor: Int32) -> Bool {
        return loomora_is_eol(family, major, minor) == 1
    }

    static var macosFamily: Int32 {
        return Int32(LOOMORA_FAMILY_MACOS)
    }

    static var eolWarning: String {
        return String(cString: loomora_eol_warning())
    }

    static var buildCompiler: String {
        return String(cString: loomora_build_compiler())
    }

    // The probe JSON line (ASCII), or nil when the backend could not build it. The first call
    // with no room measures, the second one fills the buffer.
    static func probeRecord(platformId: String, input: String, displayedText: String,
                            displayed: Bool, displayMs: UInt32, rssKb: Int64,
                            eolWarningShown: Bool, timestampUtc: String) -> String? {
        func call(_ buffer: UnsafeMutablePointer<UInt8>?, _ capacity: Int) -> Int {
            return loomora_probe_record(platformId, Int32(LOOMORA_UI_MODERN),
                                        Int32(LOOMORA_CORE_NATIVE), input, displayedText,
                                        displayed ? 1 : 0, displayMs, rssKb,
                                        eolWarningShown ? 1 : 0, "", timestampUtc,
                                        buffer, capacity)
        }
        let needed = call(nil, 0)
        if needed == 0 {
            return nil
        }
        var line = [UInt8](repeating: 0, count: needed)
        let written = line.withUnsafeMutableBufferPointer { buffer in
            return call(buffer.baseAddress, buffer.count)
        }
        if written != needed {
            return nil
        }
        return String(decoding: line, as: UTF8.self)
    }
}
