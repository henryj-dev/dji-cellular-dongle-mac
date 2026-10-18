// Menu bar only app (LSUIElement): status, live rate and pause/resume/reconnect.
import SwiftUI

@main
struct DJIDongleApp: App {
    @StateObject private var model = AgentModel()

    var body: some Scene {
        MenuBarExtra {
            MenuView(model: model)
        } label: {
            Image(systemName: menuBarSymbol(model.status, now: model.now))
                .accessibilityLabel(Text(verbatim: L("app.accessibility")))
        }
        .menuBarExtraStyle(.window)
    }
}
