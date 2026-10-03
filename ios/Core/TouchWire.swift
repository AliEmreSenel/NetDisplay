// SPDX-License-Identifier: GPL-2.0-only
import Foundation

struct TouchContact {
    let slot: Int
    let x: Int32, y: Int32, pressure: Int32, major: Int32
}
enum TouchWire {
    static func event(_ type: UInt16, _ code: UInt16, _ value: Int32) -> ControlMessage {
        var w = ByteWriter()
        w.data.append(contentsOf: [3, 0]) // NDC_DEV_TOUCHSCREEN
        w.u16(type); w.u16(code); w.u16(0); w.u32(UInt32(bitPattern: value))
        return ControlMessage(.input, w.data)
    }
    // A complete type-B frame, with stable slots and explicit contact lifetimes.
    static func frame(contacts: [TouchContact], began: Set<Int>, ended: Set<Int>) -> [ControlMessage] {
        // BTN_TOUCH is first for Linux mousedev compatibility.
        var events = [event(1, 330, contacts.isEmpty ? 0 : 1)]
        for slot in ended.sorted() {
            events += [event(3, 47, Int32(slot)), event(3, 57, -1)]
        }
        for c in contacts.sorted(by: { $0.slot < $1.slot }) {
            events.append(event(3, 47, Int32(c.slot)))
            if began.contains(c.slot) { events.append(event(3, 57, Int32(c.slot))) }
            events += [event(3, 53, c.x), event(3, 54, c.y),
                       event(3, 58, c.pressure), event(3, 48, c.major)]
        }
        if let c = contacts.min(by: { $0.slot < $1.slot }) {
            events += [event(3, 0, c.x), event(3, 1, c.y)]
        }
        events.append(event(0, 0, 0)) // SYN_REPORT
        return events
    }
    static func position(x: Double, y: Double, width: Double, height: Double,
                         sourceWidth: Double, sourceHeight: Double, clamp: Bool) -> (Int32, Int32)? {
        guard width > 0, height > 0, sourceWidth > 0, sourceHeight > 0 else { return nil }
        let scale = min(width / sourceWidth, height / sourceHeight)
        let w = sourceWidth * scale, h = sourceHeight * scale
        let u = (x - (width - w) / 2) / w, v = (y - (height - h) / 2) / h
        guard clamp || (u >= 0 && u <= 1 && v >= 0 && v <= 1) else { return nil }
        return (Int32((min(1, max(0, u)) * 65535).rounded()),
                Int32((min(1, max(0, v)) * 65535).rounded()))
    }
}
