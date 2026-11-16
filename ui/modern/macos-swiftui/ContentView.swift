// The window: end-of-life banner (when needed), text field, greeting in the middle. Colours come
// from the system semantic colours so dark mode and the increased-contrast setting are respected.
import AppKit
import SwiftUI

struct EolBanner: View {
    let message: String
    let onClose: () -> Void

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundColor(.orange)
            Text(verbatim: message)
                .foregroundColor(.primary)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading)
            Button(action: onClose) {
                Image(systemName: "xmark")
            }
            .buttonStyle(BorderlessButtonStyle())
            .help("Dismiss")
        }
        .padding(8)
        .background(Color(NSColor.controlBackgroundColor))
        .overlay(RoundedRectangle(cornerRadius: 6).stroke(Color.orange, lineWidth: 1))
        .cornerRadius(6)
    }
}

struct ContentView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        VStack(spacing: 12) {
            if let error = state.abiError {
                Text(verbatim: error)
                    .foregroundColor(.primary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                if state.eolWarningVisible {
                    EolBanner(message: state.eolWarning) {
                        state.eolWarningVisible = false
                    }
                }
                TextField("Your text", text: $state.input)
                    .textFieldStyle(RoundedBorderTextFieldStyle())
                    .onChange(of: state.input) { _ in
                        state.limitInput()
                    }
                greeting
            }
        }
        .padding(12)
        .frame(minWidth: 400, idealWidth: 480, minHeight: 200, idealHeight: 240)
        .onAppear {
            Probe.windowAppeared(state)
        }
    }

    // Centred, large, wrapped; the scroll view takes over for a long greeting.
    private var greeting: some View {
        GeometryReader { proxy in
            ScrollView {
                Text(verbatim: state.greetingText)
                    .font(.system(size: 28))
                    .foregroundColor(.primary)
                    .multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, minHeight: proxy.size.height)
            }
        }
    }
}
