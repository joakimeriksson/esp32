#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The reference for test_dock's lines: fm1-dock's tools/hbdeploy.py on the same modules.

  dock_ref.py FM1_DOCK_TOOLS_DIR MODULE.dock...
"""
import struct
import sys
import zlib

sys.path.insert(0, sys.argv[1])
import hbdeploy as h  # noqa: E402

for path in sys.argv[2:]:
    blob = open(path, "rb").read()
    name, version = h.module_info(blob)
    abi, flags = struct.unpack_from("<II", blob, 4)
    last = (h.RUN, h.run_payload(1)) if flags & 0x100 else None   # a probe: RUN (id 1) instead of COMMIT
    frames = b"".join(h.frame(cmd, i & 0x7F, payload) for i, (cmd, payload) in enumerate(h.deploy_frames(blob, last)))
    print(f"{path.split('/')[-1]}: name={name} version={version} abi={abi} flags={flags:#x} n={len(blob)} "
          f"frames={len(frames)}:{zlib.crc32(frames):08x}")
