// SPDX-License-Identifier: GPL-2.0-only
import SwiftUI
import UIKit

@MainActor
struct TouchSurface: UIViewRepresentable {
    let width: UInt16, height: UInt16
    let send: ([ControlMessage]) -> Void
    func makeUIView(context: Context) -> ForwardingTouchView {
        let view = ForwardingTouchView()
        view.isMultipleTouchEnabled = true
        view.backgroundColor = .clear
        view.sourceWidth = Double(width); view.sourceHeight = Double(height); view.send = send
        return view
    }
    func updateUIView(_ view: ForwardingTouchView, context: Context) {
        view.sourceWidth = Double(width); view.sourceHeight = Double(height); view.send = send
    }
    static func dismantleUIView(_ view: ForwardingTouchView, coordinator: ()) { view.releaseAll() }
}

@MainActor
final class ForwardingTouchView: UIView {
    var sourceWidth = 1280.0, sourceHeight = 720.0
    var send: (([ControlMessage]) -> Void)?
    private var previousSize = CGSize.zero
    private var slots: [UITouch: Int] = [:]
    private var contacts: [Int: TouchContact] = [:]
    private func contact(_ touch: UITouch, slot: Int, clamp: Bool) -> TouchContact? {
        let p = touch.location(in: self)
        guard let (x, y) = TouchWire.position(x: Double(p.x), y: Double(p.y),
            width: Double(bounds.width), height: Double(bounds.height), sourceWidth: sourceWidth,
            sourceHeight: sourceHeight, clamp: clamp) else { return nil }
        let pressure = touch.maximumPossibleForce > 0 ? touch.force / touch.maximumPossibleForce : 1
        return TouchContact(slot: slot, x: x, y: y,
            pressure: Int32(min(255, max(0, pressure * 255))),
            major: Int32(min(255, max(0, touch.majorRadius * 2))))
    }
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        var began = Set<Int>()
        for touch in touches {
            guard let slot = (0..<10).first(where: { !slots.values.contains($0) }),
                  let c = contact(touch, slot: slot, clamp: false) else { continue }
            slots[touch] = slot; contacts[slot] = c; began.insert(slot)
        }
        if !began.isEmpty { emit(began: began) }
    }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        for touch in touches {
            if let slot = slots[touch] { contacts[slot] = contact(touch, slot: slot, clamp: true) }
        }
        emit()
    }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) { end(touches) }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { end(touches) }
    private func end(_ touches: Set<UITouch>) {
        var ended = Set<Int>()
        for touch in touches {
            if let slot = slots.removeValue(forKey: touch) { contacts.removeValue(forKey: slot); ended.insert(slot) }
        }
        if !ended.isEmpty { emit(ended: ended) }
    }
    func releaseAll() {
        let ended = Set(slots.values); slots.removeAll(); contacts.removeAll()
        if !ended.isEmpty { emit(ended: ended) }
    }
    override func layoutSubviews() {
        super.layoutSubviews()
        if bounds.size != previousSize { releaseAll(); previousSize = bounds.size }
    }
    private func emit(began: Set<Int> = [], ended: Set<Int> = []) {
        send?(TouchWire.frame(contacts: Array(contacts.values), began: began, ended: ended))
    }
}
