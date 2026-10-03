#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Small stereo Linux renderer driven by NetDisplay iOS motion. Requires python3-tk."""
from __future__ import annotations
import math
import time
import tkinter as tk
from motion_protocol import arguments, make_receiver, rotate


def main():
    p = arguments(__doc__)
    p.add_argument("--width", type=int, default=1920)
    p.add_argument("--height", type=int, default=1080)
    p.add_argument("--fov", type=float, default=90, help="vertical projection field of view, degrees")
    p.add_argument("--ipd-mm", type=float, default=64, help="virtual stereo camera separation; tune to your eyes")
    args = p.parse_args()
    if not (320 <= args.width <= 4096 and 240 <= args.height <= 2160 and 30 <= args.fov <= 130 and 40 <= args.ipd_mm <= 85):
        p.error("Invalid size/FOV/IPD")
    try:
        rx = make_receiver(args)
    except (ValueError, OSError) as exc:
        p.error(str(exc))
    root = tk.Tk(className="NetDisplayPoseDemo")
    root.title("NetDisplay - stereo pose demo")
    root.geometry(f"{args.width}x{args.height}")
    canvas = tk.Canvas(root, background="#09111a", highlightthickness=0)
    canvas.pack(fill="both", expand=True)
    fullscreen = False
    def toggle(_=None):
        nonlocal fullscreen
        fullscreen = not fullscreen; root.attributes("-fullscreen", fullscreen)
    root.bind("<F11>", toggle)
    root.bind("<Escape>", lambda _: root.destroy())
    boxes = [(-1.1, 0, -3.5, "#ed785c"), (0, 0.2, -4.5, "#4cceb8"),
             (1.1, 0, -3.5, "#6aabed"), (-3,0,-7,"#d3b751"), (3,0,-7,"#b788d5")]
    edges = [(0,1),(1,3),(3,2),(2,0),(4,5),(5,7),(7,6),(6,4),(0,4),(1,5),(2,6),(3,7)]
    deadline = time.monotonic()
    def draw():
        nonlocal deadline
        pose = rx.drain()
        q = pose.quaternion if pose else (0,0,0,1)
        inverse = (-q[0],-q[1],-q[2],q[3])
        width, height = max(2,canvas.winfo_width()), max(2,canvas.winfo_height())
        half = width/2
        focal = height/(2*math.tan(math.radians(args.fov)/2))
        canvas.delete("all")
        def project(point, eye):
            eye_position = rotate(q, ((-1 if eye == 0 else 1)*args.ipd_mm/2000,0,0))
            camera = rotate(inverse, tuple(point[i]-eye_position[i] for i in range(3)))
            if camera[2] > -0.10:
                return None
            x = half*(eye+0.5) + camera[0]*focal/-camera[2]
            y = height/2 - camera[1]*focal/-camera[2]
            if not half*eye <= x <= half*(eye+1) or not 0 <= y <= height:
                return None
            return x,y
        def line(a,b,eye,color,thickness=1):
            A,B=project(a,eye),project(b,eye)
            if A is not None and B is not None:
                canvas.create_line(*A,*B,fill=color,width=thickness)
        for eye in (0,1):
            for x in range(-8,9):
                for z in range(-12,0):
                    line((x,-1.5,z),(x,-1.5,z-1),eye,"#29434e")
                    line((x,-1.5,z),(x+1,-1.5,z),eye,"#29434e")
            for x,y,z,color in boxes:
                vertices=[(x+sx*.4,y+sy*.4,z+sz*.4) for sz in (-1,1) for sy in (-1,1) for sx in (-1,1)]
                for a,b in edges: line(vertices[a],vertices[b],eye,color,3)
            canvas.create_text(half*(eye+0.5),30,text="LEFT" if eye==0 else "RIGHT",fill="#bcdde5",font=("Sans",18))
            text = "Waiting for authenticated iPhone motion" if pose is None else f"3DoF | seq {pose.sequence} | local receive age {(time.monotonic()-pose.received_at)*1000:.0f} ms"
            canvas.create_text(half*(eye+0.5),height-30,text=text,fill="#8fa5af",font=("Sans",12))
        canvas.create_line(half,0,half,height,fill="#000000",width=2)
        deadline += 1/60
        if deadline < time.monotonic(): deadline = time.monotonic()
        root.after(max(1, int((deadline-time.monotonic())*1000)), draw)
    print("Move this window to the NetDisplay Hyprland output, then press F11. Escape closes it.")
    try:
        draw(); root.mainloop()
    finally:
        rx.close()

if __name__ == "__main__":
    main()
