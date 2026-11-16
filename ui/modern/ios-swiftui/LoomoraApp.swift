// Entry point of the iOS Modern UI (SwiftUI App lifecycle).
import SwiftUI

@main
struct LoomoraApp: App {
    init() {
        Probe.start()
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}
