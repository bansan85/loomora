// Entry point of the iOS Modern UI (SwiftUI App lifecycle).
import SwiftUI

@main
struct LoomoraApp: App {
    init() {
        Probe.start()
        let project = Probe.projectOptions
        if project.badUsage {
            // Two modes at once, or a project mode with -probe or without its path: nothing is written.
            exit(2)
        }
        if let mode = project.mode, let path = project.path {
            // -validate_project / -roundtrip_project: no screen, the line goes to NSLog.
            exit(Probe.runProject(mode, path: path))
        }
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}
