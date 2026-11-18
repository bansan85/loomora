// Loomora Modern UI for macOS: Swift and SwiftUI (App lifecycle), macOS 11 API only. No
// application logic: the greeting, the end-of-life test and its warning text and the probe line
// all come from the backend through Native.swift.
import AppKit
import SwiftUI

// What the window shows and what the probe reports: the field's text and the banner.
final class AppState: ObservableObject {
    static let maxInputCharacters = 1000

    @Published var input: String
    @Published var eolWarningVisible: Bool
    let eolWarning: String
    let abiError: String?

    init(initialInput: String) {
        let abiError = AppState.abiMismatchMessage()
        self.abiError = abiError
        input = AppState.limited(initialInput)
        eolWarning = abiError == nil ? Native.eolWarning : ""
        if abiError == nil {
            let version = SystemInfo.version
            eolWarningVisible = Native.isEndOfLife(family: Native.macosFamily,
                                                   major: version.major, minor: version.minor)
        } else {
            eolWarningVisible = false
        }
    }

    // Text of the greeting as displayed: the backend's, or a readable error.
    var greetingText: String {
        switch Native.greet(input) {
        case .success(let text):
            return text
        case .failure(let error):
            return "The greeting could not be computed (error \(error.code))."
        }
    }

    // The limit counts characters (Unicode scalars), like the other UIs, not bytes.
    func limitInput() {
        if input.unicodeScalars.count > AppState.maxInputCharacters {
            input = AppState.limited(input)
        }
    }

    static func limited(_ text: String) -> String {
        let scalars = text.unicodeScalars
        if scalars.count <= maxInputCharacters {
            return text
        }
        return String(String.UnicodeScalarView(scalars.prefix(maxInputCharacters)))
    }

    static func abiMismatchMessage() -> String? {
        if Native.abiVersionMatches {
            return nil
        }
        return "Backend ABI version \(Native.abiVersion) does not match the version "
            + "\(Native.expectedAbiVersion) this application expects."
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    // Closing the window ends the program with status 0.
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        return true
    }
}

@main
struct LoomoraApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var state: AppState

    init() {
        Probe.start()
        let options = Probe.options
        if options.badUsage {
            Probe.writeError("usage: Loomora [--probe [--probe-input <utf8>] | --validate-project <path> | "
                             + "--roundtrip-project <path>]")
            exit(2)
        }
        if options.enabled || options.projectMode != nil, let message = AppState.abiMismatchMessage() {
            Probe.writeError("loomora: " + message)
            exit(1)
        }
        if let mode = options.projectMode, let path = options.projectPath {
            // --validate-project / --roundtrip-project: no window, the line goes to stdout.
            exit(Probe.runProject(mode, path: path))
        }
        _state = StateObject(wrappedValue: AppState(initialInput: options.input ?? "Coucou"))
    }

    var body: some Scene {
        WindowGroup("Loomora") {
            ContentView()
                .environmentObject(state)
        }
    }
}
