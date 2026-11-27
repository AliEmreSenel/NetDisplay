#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the real one-shot receiver and source without video devices."""

import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time


def wait_for_source(source, port):
    deadline = time.monotonic() + 5
    while source.poll() is None and time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.02)
    raise RuntimeError("Source failed to start")


# Keep the source away from any desktop session the test runs inside.
SOURCE_ENV = {k: v for k, v in os.environ.items()
              if k not in ("WAYLAND_DISPLAY", "WAYLAND_SOCKET",
                           "HYPRLAND_INSTANCE_SIGNATURE")}


def main(source_binary, receiver_binary):
    with tempfile.TemporaryDirectory() as directory, \
            socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as occupied_video_port:
        root = Path(directory)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        occupied_video_port.bind(("127.0.0.1", 0))
        video_port = occupied_video_port.getsockname()[1]
        if video_port == port:
            # Pick another TCP port for the source's combined discovery and control port.
            with socket.socket() as old_reservation, socket.socket() as reservation:
                old_reservation.bind(("127.0.0.1", port))
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]

        psk = root / "psk"
        psk.write_text(bytes(range(32)).hex() + "\n")
        server_config = root / "server.conf"
        server_config.write_text(
            f"listen=127.0.0.1\nport={port}\nvideo_port={video_port}\n"
            f"psk_file={psk}\nframe_encryption=required\n"
            "brightness_device=none\nmax_displays_per_client=1\n"
            "connect_cmd=exit 42\n"
        )
        client_config = root / "client.conf"
        client_prefix = (
            f"host=127.0.0.1\nport={port}\nframe_encryption=1\n"
            "drm_device=/dev/nonexistent\nvaapi_device=/dev/nonexistent\n"
            "display=TEST-1,1920x1080@60\ndisplay=TEST-2,1280x720@30\n"
        )
        client_config.write_text(client_prefix + f"psk_file={psk}\n")
        wrong_psk = root / "wrong-psk"
        wrong_psk.write_text(bytes(reversed(range(32))).hex() + "\n")
        wrong_config = root / "wrong.conf"
        wrong_config.write_text(client_prefix + f"psk_file={wrong_psk}\n")

        with (root / "source.log").open("w+") as source_log:
            source = subprocess.Popen(
                [source_binary, str(server_config)],
                stdout=source_log, stderr=subprocess.STDOUT, text=True,
                env=SOURCE_ENV,
            )
            try:
                wait_for_source(source, port)
                rejected = subprocess.run(
                    [receiver_binary, "--network-test", str(wrong_config)],
                    capture_output=True, text=True, timeout=15,
                )
                assert rejected.returncode != 0, rejected.stderr
                assert "source did not accept the network speed test" in rejected.stderr
                assert "network-test uplink" not in rejected.stderr
                result = subprocess.run(
                    [receiver_binary, "--network-test", str(client_config)],
                    capture_output=True, text=True, timeout=20,
                )
                assert result.returncode == 0, result.stderr
                assert result.stderr.count("testing network link") == 1, result.stderr
                for direction in ("uplink", "video-downlink"):
                    reports = re.findall(
                        rf"network-test {direction} frame=\d+ .+", result.stderr
                    )
                    assert len(reports) == 5, result.stderr
                    assert re.search(r"frame=4096.*complete=[1-9]\d*/", reports[0]), result.stderr
            except Exception:
                source_log.seek(0)
                print(source_log.read(), file=sys.stderr)
                raise
            finally:
                source.terminate()
                status = source.wait(timeout=5)
            assert status == 0, f"source exited {status} on SIGTERM"
    print("One authenticated, encrypted link test completed using ephemeral ports and no displays")


if __name__ == "__main__":
    main(*sys.argv[1:])
