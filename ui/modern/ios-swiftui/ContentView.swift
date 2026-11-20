// The only screen of the iOS Modern UI: a TextField pre-filled with "Coucou" (limited to 1,000
// characters), a Text with the greeting of the backend, a banner for the end-of-life warning. No
// application logic: the greeting, the end-of-life verdict and its text come from Native (the C API
// of the backend); this file wires controls and converts text.
import SwiftUI

final class GreetingModel: ObservableObject {
    static let defaultInput = "Coucou"
    static let greetingError = "(error)"

    @Published var input: String
    let abiMatches: Bool
    let eolWarning: String?

    init() {
        input = Probe.limited(Probe.inputOverride ?? GreetingModel.defaultInput)
        let matches = Native.abiVersion() == Native.expectedAbiVersion
        abiMatches = matches
        let version = ProcessInfo.processInfo.operatingSystemVersion
        let endOfLife = Native.isEol(family: Native.familyIos, major: Int32(version.majorVersion),
                                     minor: Int32(version.minorVersion))
        eolWarning = matches && endOfLife ? Native.eolWarning() : nil
    }

    // Asked to the backend at each change of the field.
    var greeting: String {
        if !abiMatches {
            return "The interface and the engine do not match (ABI version)."
        }
        return Native.greet(input) ?? GreetingModel.greetingError
    }
}

struct ContentView: View {
    @StateObject private var model = GreetingModel()

    var body: some View {
        VStack(spacing: 16) {
            if let warning = model.eolWarning {
                Text(warning)
                    .font(.callout)
                    .foregroundColor(.black)
                    .multilineTextAlignment(.center)
                    .padding(8)
                    .frame(maxWidth: .infinity)
                    .background(Color(red: 1.0, green: 0.85, blue: 0.3))
                    .accessibilityIdentifier("eol-warning")
            }
            Spacer()
            Text(model.greeting)
                .font(.title)
                .foregroundColor(.primary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: .infinity)
                .accessibilityIdentifier("greeting")
            TextField("", text: $model.input)
                .disableAutocorrection(true)
                .accessibilityIdentifier("input")
                .onChange(of: model.input) { newValue in
                    let limited = Probe.limited(newValue)
                    if limited != newValue {
                        model.input = limited
                    }
                }
            Spacer()
        }
        .padding()
        .onAppear {
            Probe.begin {
                (input: model.input, message: model.greeting, eolShown: model.eolWarning != nil)
            }
        }
    }
}
