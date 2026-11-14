import SwiftUI

@main
struct SidespeakerApp: App {
    @StateObject private var rx = Receiver()

    var body: some Scene {
        WindowGroup {
            VStack(spacing: 16) {
                Text("Sidespeaker").font(.largeTitle)
                Text(rx.status).foregroundColor(.secondary).multilineTextAlignment(.center)
            }
            .onAppear { rx.start() }
        }
    }
}
