#!/usr/bin/env python3
"""Unpack, tweak and repack an unsigned CrossDesk iOS .ipa.

Runs on any OS (pure Python, no Xcode): it edits the bundle's Info.plist and
repackages Payload/ with forward slashes, which is all a sideloading tool such
as Sideloadly or AltStore needs before it signs with your Apple ID.

Usage:
  python scripts/ios/repack-ipa.py IN.ipa OUT.ipa
  python scripts/ios/repack-ipa.py IN.ipa OUT.ipa --display-name "CrossDesk 照片版"
  python scripts/ios/repack-ipa.py IN.ipa OUT.ipa --version 1.7.6-photo --bundle-id com.example.crossdesk

Limitations, by design: this tool only changes bundle metadata and packaged
resources. It cannot add a new screen, because the UI is compiled SwiftUI and
only the Apple toolchain can produce new arm64-apple-ios code.
"""
import argparse
import pathlib
import plistlib
import tempfile
import zipfile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--display-name", default=None)
    parser.add_argument("--version", default=None, help="CFBundleShortVersionString")
    parser.add_argument("--build", default=None, help="CFBundleVersion")
    parser.add_argument("--bundle-id", default=None)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as work:
        root = pathlib.Path(work)
        with zipfile.ZipFile(args.source) as archive:
            archive.extractall(root)
        apps = sorted((root / "Payload").glob("*.app"))
        if not apps:
            raise SystemExit("no Payload/*.app inside " + str(args.source))
        app = apps[0]
        plist_path = app / "Info.plist"
        with open(plist_path, "rb") as handle:
            info = plistlib.load(handle)
        before = {key: info.get(key) for key in (
            "CFBundleIdentifier", "CFBundleShortVersionString", "CFBundleVersion",
            "CFBundleDisplayName", "MinimumOSVersion")}
        if args.display_name:
            info["CFBundleDisplayName"] = args.display_name
        if args.version:
            info["CFBundleShortVersionString"] = args.version
        if args.build:
            info["CFBundleVersion"] = args.build
        if args.bundle_id:
            info["CFBundleIdentifier"] = args.bundle_id
        with open(plist_path, "wb") as handle:
            plistlib.dump(info, handle)
        after = {key: info.get(key) for key in before}
        print("bundle:", app.name)
        for key in before:
            if before[key] != after[key]:
                print(f"  {key}: {before[key]} -> {after[key]}")
            else:
                print(f"  {key}: {after[key]}")

        if args.output.exists():
            args.output.unlink()
        files = [p for p in root.rglob("*") if p.is_file()]
        with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as out:
            for path in sorted(files):
                out.write(path, path.relative_to(root).as_posix())
        print(f"{args.output}  entries={len(files)}  "
              f"size={args.output.stat().st_size / 1048576:.2f} MB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
