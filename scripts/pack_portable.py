#!/usr/bin/env python3
"""Stage the portable payload as a complete, verified installed layout.

The output tree matches a fresh setup installation exactly: a stable launcher
``pulse.exe``, the ``dialog-current.json`` version pointer and every
application/dialog file inside the content-addressed
``DialogBridge/<manifest hash>/`` bundle. Keeping the same verified layout
keeps the dialog take-over feature fully available from a portable,
extraction-only deployment.

Only the Python standard library is used.
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=".", help="repository root")
    ap.add_argument("--build", default="build-ci",
                    help="build directory (relative to root or absolute)")
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", required=True,
                    help="output directory (relative to root or absolute); must not exist")
    ap.add_argument("--extension-store", type=Path, default=None,
                    help="optional pre-built extension store to ship at the payload root")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    build = Path(args.build)
    actual_build = build if build.is_absolute() else root / build
    out = Path(args.out)
    out = out if out.is_absolute() else root / out

    sys.path.insert(0, str(root / "tools" / "pack_installer"))
    import pack_installer
    items = pack_installer.collect(root, build)

    sys.path.insert(0, str(root / "tools" / "pack_dialog_experiment"))
    import pack as dialog_pack
    machine = dialog_pack.PE(actual_build / "pulse.exe").machine
    arch = next((name for name, value in dialog_pack.MACHINES.items() if value == machine), None)
    if arch is None:
        raise SystemExit("unsupported Pulse architecture for dialog bundle")

    # package_installed never overwrites and fully verifies the tree. The
    # checksum inventory is written afterwards, so it stays outside the
    # verified bundle set and never changes the packaged contents.
    out.parent.mkdir(parents=True, exist_ok=True)
    dialog_pack.package_installed(actual_build, out, args.version, arch, root, items)

    if args.extension_store is not None:
        # Ship a fully built extension store (registry.json plus content-addressed
        # binaries/ and versions/) at the payload root. It stays outside the
        # verified DialogBridge bundle, so the bundle hash is unaffected, and the
        # stable launcher advertises it to the host for out-of-the-box loading.
        shutil.copytree(args.extension_store, out / "extensions")

    files = []
    for path in sorted(p for p in out.rglob("*") if p.is_file()):
        relative = path.relative_to(out).as_posix()
        if relative != "SHA256SUMS.json":
            files.append({"name": relative, "sha256": dialog_pack.digest(path)})
    with (out / "SHA256SUMS.json").open("x", encoding="utf-8", newline="\n") as stream:
        json.dump({"version": args.version, "files": files}, stream, indent=2)
        stream.write("\n")
    print(f"portable staged: {len(files)} hashed files under {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
