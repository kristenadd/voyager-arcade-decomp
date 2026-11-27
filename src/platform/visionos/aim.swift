// Apple Vision Pro: where PlayStation VR2 Sense controllers point on the window.
//
// An invisible RealityKit view covers the window. Each spatial controller gets
// an AnchorEntity on its "aim" location; the point where the aim's forward ray
// crosses the window's surface (z = 0 in the view, its centre at the origin)
// is where that controller points. The anchors' transforms stay hidden until
// accessory tracking is allowed and a SpatialTrackingSession runs, so the
// first controller seen asks for the permission (NSAccessoryTrackingUsageDescription).
//
// The C side (platform/apple/controllers.mm) calls voy_vision_aim_attach once
// with the UIWindow and voy_vision_aim_poll once a frame.

import ARKit
import GameController
import RealityKit
import SwiftUI
import UIKit

/// Only with VOY_INPUT_LOG set, like the C side's input_log.
private let aimLogging = ProcessInfo.processInfo.environment["VOY_INPUT_LOG"] != nil
private func aimLog(_ text: String) {
    if aimLogging { FileHandle.standardError.write(Data("[input] aim: \(text)\n".utf8)) }
}

@MainActor
final class SpatialAim: ObservableObject {
    let root = Entity()
    var windowSize = SIMD2<Float>(0, 0)  // metres

    private struct Hand {
        var anchor: AnchorEntity?
        var chirality: Int32 = 0  // 0 unknown, 1 left, 2 right
    }
    /// GCController is not Sendable; the loader only reads it to find the accessory.
    private struct Device: @unchecked Sendable {
        let controller: GCController
    }
    private nonisolated static func source(for device: Device) async throws
        -> AnchoringComponent.AccessoryAnchoringSource {
        try await AnchoringComponent.AccessoryAnchoringSource(device: device.controller)
    }

    private var hands: [ObjectIdentifier: Hand] = [:]
    private var controllers: [ObjectIdentifier: GCController] = [:]
    private var session: SpatialTrackingSession?
    private var started = false
    private var trackingLive = false
    private var trackingBusy = false
    // The aim location's -Z points about 22° above where a Sense controller is
    // held as a gun (measured on the headset for Keisho), so the ray is tipped
    // down about the controller's own X axis.
    private static let pitchCos = Float(cos(22.0 * Double.pi / 180.0))
    private static let pitchSin = Float(sin(22.0 * Double.pi / 180.0))

    func start() {
        guard !started else { return }
        started = true
        for name in [NSNotification.Name.GCControllerDidConnect, .GCControllerDidDisconnect] {
            NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.refresh() }
            }
        }
        NotificationCenter.default.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil,
                                               queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.runTracking(attempt: 0) }
        }
        refresh()
        runTracking(attempt: 0)
    }

    /// Ask for accessory tracking, then run the session that reveals the
    /// anchors' transforms. Tried again for a while if the system says no,
    /// which it does before the window has focus or while the prompt is up.
    private func runTracking(attempt: Int) {
        guard !trackingLive, !trackingBusy else { return }
        trackingBusy = true
        Task { @MainActor in
            let arkit = ARKitSession()
            let status = await arkit.requestAuthorization(for: [.accessoryTracking])
            aimLog("accessory tracking permission: \(status[.accessoryTracking].map { "\($0)" } ?? "?")")
            let session = self.session ?? SpatialTrackingSession()
            self.session = session
            let unavailable = await session.run(SpatialTrackingSession.Configuration(tracking: [.accessory]))
            self.trackingBusy = false
            if let unavailable, unavailable.anchor.contains(.accessory) {
                aimLog("accessory tracking unavailable (attempt \(attempt + 1))")
                if attempt < 20 {
                    try? await Task.sleep(for: .seconds(3))
                    self.runTracking(attempt: attempt + 1)
                }
                return
            }
            self.trackingLive = true
            aimLog("tracking session running")
        }
    }

    private func refresh() {
        let spatial = GCController.controllers().filter { $0.productCategory == GCProductCategorySpatialController }
        let live = Set(spatial.map { ObjectIdentifier($0) })
        for (id, hand) in hands where !live.contains(id) {
            hand.anchor?.removeFromParent()
            hands[id] = nil
            controllers[id] = nil
        }
        for controller in spatial {
            let id = ObjectIdentifier(controller)
            guard hands[id] == nil else { continue }
            hands[id] = Hand()
            controllers[id] = controller
            let device = Device(controller: controller)
            Task { @MainActor in
                do {
                    let source = try await SpatialAim.source(for: device)
                    guard let location = source.locationName(named: "aim"), self.hands[id] != nil else {
                        aimLog("\(controller.vendorName ?? "?") has no aim location")
                        return
                    }
                    let anchor = AnchorEntity(.accessory(from: source, location: location), trackingMode: .predicted)
                    self.root.addChild(anchor)
                    var chirality: Int32 = 0
                    switch source.underlyingAccessory?.inherentChirality {
                    case .left?: chirality = 1
                    case .right?: chirality = 2
                    default: chirality = 0
                    }
                    self.hands[id]?.anchor = anchor
                    self.hands[id]?.chirality = chirality
                    aimLog("tracking \(controller.vendorName ?? "?") (hand \(chirality))")
                } catch {
                    aimLog("could not track \(controller.vendorName ?? "?"): \(error)")
                }
            }
        }
    }

    /// Up to `max` entries: the controller (unretained), its hand, whether it
    /// is tracked and pointing at the window's plane, and where on the window
    /// (0..1 from the top left; outside that is off the window).
    func sample(controllers out: UnsafeMutablePointer<UnsafeRawPointer?>,
                values: UnsafeMutablePointer<Float>, max: Int) -> Int {
        var count = 0
        for (id, hand) in hands {
            guard count < max, let controller = controllers[id] else { continue }
            var tracked: Float = 0
            var x: Float = -1
            var y: Float = -1
            if let anchor = hand.anchor, anchor.isAnchored, windowSize.x > 0, windowSize.y > 0 {
                let m = anchor.transformMatrix(relativeTo: root)
                let origin = SIMD3<Float>(m.columns.3.x, m.columns.3.y, m.columns.3.z)
                let back = simd_normalize(SIMD3<Float>(m.columns.2.x, m.columns.2.y, m.columns.2.z))
                let up = simd_normalize(SIMD3<Float>(m.columns.1.x, m.columns.1.y, m.columns.1.z))
                let ahead = -back * SpatialAim.pitchCos - up * SpatialAim.pitchSin
                if ahead.z < -0.05, origin.z > 0 {  // in front of the window, pointing at it
                    let t = -origin.z / ahead.z
                    let hit = origin + ahead * t
                    x = 0.5 + hit.x / windowSize.x
                    y = 0.5 - hit.y / windowSize.y
                    tracked = 1
                }
            }
            out[count] = UnsafeRawPointer(Unmanaged.passUnretained(controller).toOpaque())
            values[count * 4 + 0] = Float(hand.chirality)
            values[count * 4 + 1] = tracked
            values[count * 4 + 2] = x
            values[count * 4 + 3] = y
            count += 1
        }
        return count
    }
}

struct AimView: View {
    @ObservedObject var aim: SpatialAim
    @Environment(\.physicalMetrics) private var metrics

    var body: some View {
        GeometryReader { proxy in
            RealityView { content in
                content.add(aim.root)
            } update: { _ in
                aim.windowSize = SIMD2<Float>(Float(metrics.convert(proxy.size.width, to: .meters)),
                                              Float(metrics.convert(proxy.size.height, to: .meters)))
            }
        }
        .allowsHitTesting(false)
    }
}

@MainActor
private enum AimLayer {
    static var aim: SpatialAim?
    static var host: UIHostingController<AimView>?
}

@_cdecl("voy_vision_aim_attach")
func voy_vision_aim_attach(_ window: UnsafeMutableRawPointer?) {
    let address = UInt(bitPattern: window)
    MainActor.assumeIsolated {
        guard let window = UnsafeMutableRawPointer(bitPattern: address), AimLayer.aim == nil else { return }
        let ui = Unmanaged<UIWindow>.fromOpaque(window).takeUnretainedValue()
        let aim = SpatialAim()
        let host = UIHostingController(rootView: AimView(aim: aim))
        host.view.backgroundColor = .clear
        host.view.isUserInteractionEnabled = false
        host.view.frame = ui.bounds
        host.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        ui.addSubview(host.view)
        AimLayer.aim = aim
        AimLayer.host = host
    }
}

@_cdecl("voy_vision_aim_poll")
func voy_vision_aim_poll(_ controllers: UnsafeMutablePointer<UnsafeRawPointer?>?,
                         _ values: UnsafeMutablePointer<Float>?, _ max: Int32) -> Int32 {
    // On the main thread, once a frame while a spatial controller is connected.
    let outAddress = UInt(bitPattern: controllers)
    let valueAddress = UInt(bitPattern: values)
    return MainActor.assumeIsolated {
        guard let aim = AimLayer.aim,
              let out = UnsafeMutablePointer<UnsafeRawPointer?>(bitPattern: outAddress),
              let values = UnsafeMutablePointer<Float>(bitPattern: valueAddress) else { return 0 }
        aim.start()
        return Int32(aim.sample(controllers: out, values: values, max: Int(max)))
    }
}
