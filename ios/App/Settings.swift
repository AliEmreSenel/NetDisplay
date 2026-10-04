// SPDX-License-Identifier: GPL-2.0-only
import Foundation
import Security

enum AuthMode: String, CaseIterable, Identifiable, Codable {
    case password = "Password", key = "32-byte key (hex)", none = "No authentication"
    var id: String { rawValue }
}
enum ViewerMode: String, CaseIterable, Identifiable, Codable {
    case stereo = "Side-by-side stereo", duplicate = "Mono in both eyes", flat = "Flat screen"
    var id: String { rawValue }
    var shaderValue: Float { switch self { case .flat: return 0; case .stereo: return 1; case .duplicate: return 2 } }
}
enum ViewerPurpose: String, CaseIterable, Identifiable, Codable {
    case touch = "Touch Display", vr = "VR / SteamVR"
    var id: String { rawValue }
}
struct AppSettings: Codable, Equatable {
    // Optional storage preserves decoding of settings saved before this choice existed.
    var savedViewerPurpose: ViewerPurpose? = nil
    var viewerPurpose: ViewerPurpose {
        get { savedViewerPurpose ?? .touch }
        set { savedViewerPurpose = newValue }
    }
    var renderMode: ViewerMode { viewerPurpose == .touch ? .flat : viewerMode }

    var host = ""
    var controlPort = 5001
    var resolution = "1280x720"
    var fps = 60
    var authMode = AuthMode.password
    var encryptVideo = false
    var allowHEVC = false
    // Optional stored fields retain compatibility with existing saved settings.
    var savedRearCameraTracking: Bool? = nil
    var rearCameraTracking: Bool {
        get { savedRearCameraTracking ?? false }
        set { savedRearCameraTracking = newValue }
    }
    var savedHeadOffsetX: Double? = nil, savedHeadOffsetY: Double? = nil, savedHeadOffsetZ: Double? = nil
    var cameraToHeadOffset: Vec3 {
        Vec3(x: savedHeadOffsetX ?? 0, y: savedHeadOffsetY ?? 0, z: savedHeadOffsetZ ?? 0)
    }
    var sendMotion = true
    var motionPort = 5010
    var cameraOnLeft = true
    var viewerMode = ViewerMode.stereo
    var lensCorrection = false
    var k1: Float = 0.22
    var k2: Float = 0.10
    var imageScale: Float = 1.0
    var lensCenterShift: Float = 0
    var verticalShift: Float = 0
    var swapEyes = false
    var showHUD = true
    var demoFOV: Float = 90
    static let resolutions = ["1280x720", "1920x1080", "2240x1080", "2532x1170"]
    var dimensions: (UInt16, UInt16) {
        let p = resolution.split(separator: "x").compactMap { UInt16($0) }
        return p.count == 2 ? (p[0], p[1]) : (1280, 720)
    }
    func validate() throws {
        let offset = cameraToHeadOffset
        guard [offset.x,offset.y,offset.z].allSatisfy({ $0.isFinite && abs($0) <= 0.5 }) else {
            throw NDError.message("Camera-to-head offsets must be finite values between -0.5 and 0.5 metres")
        }
        guard SocketIO.isIPv4(host) else { throw NDError.message("Enter the Linux computer's IPv4 address on the USB/hotspot interface, not the iPhone's gateway address.") }
        guard (1...65535).contains(controlPort), (1...65535).contains(motionPort) else {
            throw NDError.message("Ports must be between 1 and 65535")
        }
        guard Self.resolutions.contains(resolution), [30, 60].contains(fps) else {
            throw NDError.message("Select a supported resolution and frame rate")
        }
        guard !encryptVideo || authMode != .none else { throw NDError.message("Video encryption requires a password or pre-shared key") }
    }
    static func load() -> AppSettings {
        guard let d = UserDefaults.standard.data(forKey: "netdisplay.settings.v1"),
              let s = try? JSONDecoder().decode(AppSettings.self, from: d) else { return AppSettings() }
        return s
    }
    func save() {
        if let d = try? JSONEncoder().encode(self) { UserDefaults.standard.set(d, forKey: "netdisplay.settings.v1") }
    }
}

enum KeyStore {
    static let service = "NetDisplay.MotionToken"
    static func readOrCreateToken() throws -> [UInt8] {
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service, kSecAttrAccount as String: "default",
            kSecReturnData as String: true, kSecMatchLimit as String: kSecMatchLimitOne]
        var result: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        if status == errSecSuccess, let d = result as? Data, d.count == 32 { return Array(d) }
        guard status == errSecItemNotFound else {
            throw NDError.message("Cannot read motion token from Keychain (\(status)). Unlock the phone and relaunch.")
        }
        var key = [UInt8](repeating: 0, count: 32)
        guard SecRandomCopyBytes(kSecRandomDefault, key.count, &key) == errSecSuccess else {
            throw NDError.message("Secure random generator failed")
        }
        let add: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service, kSecAttrAccount as String: "default",
            kSecAttrAccessible as String: kSecAttrAccessibleWhenUnlockedThisDeviceOnly,
            kSecValueData as String: Data(key)]
        let saved = SecItemAdd(add as CFDictionary, nil)
        guard saved == errSecSuccess else { throw NDError.message("Cannot save motion token (\(saved))") }
        return key
    }
}
