#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The reference for test_fm1's package lines: fm1-dock's tools/fm1_install.py on the same packages.

  pkg_ref.py FM1_DOCK_TOOLS_DIR PACKAGE.fwsc...
"""
import sys

sys.path.insert(0, sys.argv[1])
import fm1_install as f  # noqa: E402


def crc(b):
    return f._crc16(b)


for path in sys.argv[2:]:
    raw = open(path, "rb").read()
    img = f.logical_image(raw)
    board = {"fm1": "fm1", "smk": "smk", None: "unknown"}[f.package_board(img)]
    resp = f.response(0x1234, img[0x100:0x100 + 512], 3)
    print(f"{path.split('/')[-1]}: product={f.product_of(raw)} n={len(img)} crc={crc(img):04x} "
          f"damage={f.package_damage(img) or 'none'} board={board} felucca={int(f.LOADER_MARK in raw)} "
          f"fm1={'refuse' if f.board_refusal('FM-1', img) else 'ok'} "
          f"smk={'refuse' if f.board_refusal('SMK-37 Pro', img) else 'ok'} "
          f"resp={len(resp)}:{crc(resp):04x}")
