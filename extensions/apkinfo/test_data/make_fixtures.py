#!/usr/bin/env python3
"""Generates synthetic APK fixtures for the Pulse apkinfo extension tests.

Builds a real ZIP (via zipfile, deflate) containing a hand-crafted binary
AndroidManifest.xml (AXML) plus resources.arsc-style filler, mirroring the
chunk layout of aapt output. Two variants: a UTF-8 string pool manifest and a
UTF-16 one, plus a deliberately corrupt manifest for the failure path.

Usage: python3 make_fixtures.py <output-dir>
"""
import os
import struct
import sys
import zipfile


def u16(value: int) -> bytes:
    return struct.pack("<H", value)


def u32(value: int) -> bytes:
    return struct.pack("<I", value)


def chunk(chunk_type: int, payload: bytes, header_size: int = 8) -> bytes:
    return u16(chunk_type) + u16(header_size) + u32(header_size + len(payload)) + payload


def string_pool(strings, utf8: bool) -> bytes:
    """ResStringPool chunk with the given strings (encoding per `utf8`)."""
    flags = 0x100 if utf8 else 0
    encoded = []
    for text in strings:
        if utf8:
            data = text.encode("utf-8")
            # u16len and u8len as 7-bit varints (both < 0x80 here), then bytes + NUL.
            encoded.append(bytes([len(text), len(data)]) + data + b"\x00")
        else:
            encoded.append(u16(len(text)) + text.encode("utf-16-le") + u16(0))
    strings_start = 28 + 4 * len(strings)
    body = b"".join(encoded)
    offsets = b"".join(
        u32(sum(len(e) for e in encoded[:i])) for i in range(len(strings))
    )
    header = (
        u16(0x0001)          # RES_STRING_POOL_TYPE
        + u16(28)            # header size
        + u32(28 + len(offsets) + len(body))  # chunk size
        + u32(len(strings))  # string count
        + u32(0)             # style count
        + u32(flags)         # flags
        + u32(strings_start)
        + u32(0)             # styles start
    )
    return header + offsets + body


def attribute(ns, name, raw, data_type, data) -> bytes:
    return u32(ns) + u32(name) + u32(raw) + u16(8) + bytes([0, data_type]) + u32(data)


def start_element(name_index, attributes) -> bytes:
    """ResXMLTree start element with the given (ns, name, raw, type, data) rows."""
    attr_bytes = b"".join(attribute(*a) for a in attributes)
    header = u16(0x0102) + u16(16) + u32(36 + len(attr_bytes))
    body = u32(1) + u32(0xFFFFFFFF)  # line number, comment
    ext = u32(0xFFFFFFFF) + u32(name_index)  # ns, name
    ext += u16(20) + u16(20) + u16(len(attributes)) + u16(0) + u16(0) + u16(0)
    return header + body + ext + attr_bytes


def end_element() -> bytes:
    # ResXMLTree_node with an empty endElementExt: header(8) + line(4) + comment(4).
    return u16(0x0103) + u16(16) + u32(16) + u32(1) + u32(0xFFFFFFFF)


def axml_manifest(utf8: bool, corrupt: bool = False) -> bytes:
    strings = [
        "manifest", "package", "versionName", "versionCode", "split",
        "uses-sdk", "minSdkVersion", "targetSdkVersion",
        "uses-permission", "name", "application", "label", "icon",
        "com.pulse.filemanager", "3.2.1",
        "android.permission.CAMERA", "android.permission.INTERNET",
        "Pulse File Manager",
        "http://schemas.android.com/apk/res/android",
    ]
    if corrupt:
        strings = ["manifest"]
    ns_none = 0xFFFFFFFF  # manifest attributes carry no namespace
    ns_android = 18       # pool index of the android namespace URI
    pool = string_pool(strings, utf8)
    resmap = chunk(0x0180, u32(0x01010001) + u32(0x0101021c) + u32(0x0101021b))
    manifest_attrs = [
        (ns_none, 1, 0xFFFFFFFF, 0x03, 13),     # package = "com.pulse.filemanager"
        (ns_android, 2, 0xFFFFFFFF, 0x03, 14),  # android:versionName = "3.2.1"
        (ns_android, 3, 0xFFFFFFFF, 0x10, 321),  # android:versionCode = 321 (typed int)
    ]
    uses_sdk_attrs = [
        (ns_android, 6, 0xFFFFFFFF, 0x10, 21),   # minSdkVersion = 21
        (ns_android, 7, 0xFFFFFFFF, 0x10, 34),   # targetSdkVersion = 34
    ]
    permission_attrs = [(ns_android, 9, 0xFFFFFFFF, 0x03, 15)]  # CAMERA
    permission2_attrs = [(ns_android, 9, 0xFFFFFFFF, 0x03, 16)]  # INTERNET
    application_attrs = [
        (ns_android, 11, 0xFFFFFFFF, 0x03, 17),  # label = "Pulse File Manager"
        (ns_android, 12, 0xFFFFFFFF, 0x01, 0x7F0E0001),  # icon = resource ref
    ]
    body = (
        start_element(0, manifest_attrs)
        + start_element(5, uses_sdk_attrs) + end_element()
        + start_element(8, permission_attrs) + end_element()
        + start_element(8, permission2_attrs) + end_element()
        + start_element(10, application_attrs) + end_element()
        + end_element()
    )
    file_header = u16(0x0003) + u16(8) + u32(8 + len(pool) + len(resmap) + len(body))
    document = file_header + pool + resmap + body
    if corrupt:
        # Truncate mid-chunk to exercise the bounds checks.
        return document[: len(document) - 9]
    return document


def write_apk(path: str, manifest: bytes) -> None:
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        info = zipfile.ZipInfo("AndroidManifest.xml")
        archive.writestr(info, manifest)
        archive.writestr(zipfile.ZipInfo("classes.dex"), b"dex\n035\x00" + b"\x00" * 64)
        archive.writestr(zipfile.ZipInfo("resources.arsc"), b"\x02" * 128)
        archive.writestr(zipfile.ZipInfo("res/mipmap/ic_launcher.png"), b"\x89PNG\r\n\x1a\n fake")
        archive.writestr(zipfile.ZipInfo("META-INF/MANIFEST.MF"), b"Manifest-Version: 1.0\n")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    write_apk(os.path.join(out, "utf8.apk"), axml_manifest(utf8=True))
    write_apk(os.path.join(out, "utf16.apk"), axml_manifest(utf8=False))
    write_apk(os.path.join(out, "corrupt.apk"), axml_manifest(utf8=True, corrupt=True))
    # A ZIP with no manifest at all.
    with zipfile.ZipFile(os.path.join(out, "nomanifest.apk"), "w") as archive:
        archive.writestr("classes.dex", b"dex")
    print(f"fixtures written to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
