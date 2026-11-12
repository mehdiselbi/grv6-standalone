import DJCore
import SwiftUI
import UniformTypeIdentifiers

@main
struct GRV6DJApp: App {
    @StateObject private var engine = Engine()
    @Environment(\.scenePhase) private var phase
    var body: some Scene {
        WindowGroup {
            XDJScreen()
                .environmentObject(engine)
                .preferredColorScheme(.dark)
                .persistentSystemOverlays(.hidden)
                .statusBarHidden()
                .onAppear { UIApplication.shared.isIdleTimerDisabled = true }   // never auto-lock while the app is open
                .onChange(of: phase) { _, p in if p == .active { UIApplication.shared.isIdleTimerDisabled = true } }
        }
    }
}
