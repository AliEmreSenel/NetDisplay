// SPDX-License-Identifier: GPL-2.0-only
import SwiftUI
import UIKit

@MainActor
final class AppDelegate: NSObject, UIApplicationDelegate {
    static var orientation: UIInterfaceOrientationMask = AppSettings.load().cameraOnLeft ? .landscapeRight : .landscapeLeft
    func application(_ application: UIApplication, supportedInterfaceOrientationsFor window: UIWindow?) -> UIInterfaceOrientationMask { Self.orientation }
    static func setOrientation(cameraOnLeft: Bool) {
        orientation = cameraOnLeft ? .landscapeRight : .landscapeLeft
        for scene in UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene }) {
            scene.windows.first?.rootViewController?.setNeedsUpdateOfSupportedInterfaceOrientations()
            scene.requestGeometryUpdate(.iOS(interfaceOrientations: orientation)) { _ in }
        }
    }
}
@main
@MainActor
struct NetDisplayApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject private var model = AppModel()
    @Environment(\.scenePhase) private var phase
    var body: some Scene {
        WindowGroup {
            MainView(model: model)
                .preferredColorScheme(.dark)
                .onChange(of: phase) { value in if value == .background { model.suspend() } }
                .onChange(of: model.settings) { value in value.save() }
                .onChange(of: model.settings.cameraOnLeft) { value in AppDelegate.setOrientation(cameraOnLeft: value) }
        }
    }
}
