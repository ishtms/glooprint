#!/usr/bin/env python3
# Copyright 2026 Ishtmeet Singh. All Rights Reserved.
"""Stage a Fab source archive; optionally validate it with installed-engine UAT.

Uses only Python's standard library. Output must be a new directory. Build products
stay outside the submission archive; Epic builds the distributed binaries.
"""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import zipfile


def prepare(args):
    root = Path(__file__).resolve().parents[1]
    output = args.output.expanduser().resolve()
    if output.exists():
        raise ValueError("Output already exists. Choose a new directory; existing results are never replaced.")
    if output == root or output in root.parents:
        raise ValueError("Output cannot contain the source repository.")
    if output.is_relative_to(root) and output.relative_to(root).parts[0] in {
        "Source", "Config", "Resources", "Documentation", "Scripts", "Submission"
    }:
        raise ValueError("Output cannot be inside release inputs or maintainer source directories.")

    host = {"Darwin": "Mac", "Windows": "Win64"}.get(platform.system())
    engine = args.engine_root.expanduser().resolve() if args.engine_root else None
    if engine:
        if not host:
            raise ValueError("Native release builds are supported on Windows and macOS only.")
        version = json.loads((engine / "Engine/Build/Build.version").read_text(encoding="utf-8-sig"))
        if (version["MajorVersion"], version["MinorVersion"]) != (5, 8):
            raise ValueError("This release targets UE 5.8. Build with an installed UE 5.8 engine.")
        uat = engine / ("Engine/Build/BatchFiles/RunUAT.bat" if host == "Win64"
                        else "Engine/Build/BatchFiles/RunUAT.sh")
        if not uat.is_file():
            raise ValueError("Installed-engine RunUAT was not found.")

    files = [root / "GlooPrint.uplugin"]
    extensions = {
        "Source": {".h", ".cpp", ".cs"},
        "Config": {".ini"},
        "Resources": {".png", ".svg"},
        "Documentation": {".txt"},
    }
    for directory, allowed in extensions.items():
        for path in sorted((root / directory).rglob("*")):
            if path.is_symlink():
                raise ValueError("Release input must not be a symlink: " + str(path))
            if path.is_file() and path.suffix in allowed:
                files.append(path)
    descriptor = json.loads(files[0].read_text())
    for key in ("CreatedBy", "DocsURL", "SupportURL", "FabURL", "EngineVersion", "VersionName"):
        if not descriptor.get(key):
            raise ValueError("Missing descriptor field: " + key)
    if descriptor["EngineVersion"] != "5.8.0":
        raise ValueError("Descriptor must target 5.8.0.")
    if descriptor.get("SupportedTargetPlatforms") != ["Win64", "Mac"]:
        raise ValueError("Release platform declarations must be Win64 and Mac.")
    if not descriptor.get("Modules") or any(
        module.get("Type") != "Editor" or module.get("PlatformAllowList") != ["Win64", "Mac"]
        for module in descriptor["Modules"]
    ):
        raise ValueError("Expected editor modules restricted to Win64 and Mac.")
    for relative in ("Resources/Icon128.png", "Config/FilterPlugin.ini", "Documentation/UserGuide.txt"):
        if root / relative not in files:
            raise ValueError("Missing release input: " + relative)
    icon = (root / "Resources/Icon128.png").read_bytes()
    if icon[:8] != b"\x89PNG\r\n\x1a\n" or icon[16:24] != b"\0\0\0\x80\0\0\0\x80":
        raise ValueError("Plugin icon must be a 128 by 128 PNG.")

    manifest = {}
    for path in files:
        relative = path.relative_to(root)
        archive_path = "GlooPrint/" + relative.as_posix()
        if len(archive_path) > 170:
            raise ValueError("Fab plugin path exceeds 170 characters: " + archive_path)
        if any(not re.fullmatch(r"[A-Za-z0-9_.]+", part) for part in relative.parts):
            raise ValueError("Unsupported release filename: " + str(relative))
        if relative.parts[0] == "Source":
            first_line = path.read_text().splitlines()[0]
            if "Copyright 2026 " + descriptor["CreatedBy"] not in first_line:
                raise ValueError("Missing publisher copyright header: " + str(relative))
        manifest[archive_path] = hashlib.sha256(path.read_bytes()).hexdigest()

    output.mkdir(parents=True)
    stage = output / "Source/GlooPrint"
    for path in files:
        destination = stage / path.relative_to(root)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, destination)

    record = {"version": descriptor["VersionName"], "engine": "5.8", "files": manifest,
              "build": {"status": "not_run"}, "editor_tests": "not_run",
              "windows_manual_testing": "pending"}
    record_path = output / "ReleaseRecord.json"

    def save_record():
        record_path.write_text(json.dumps(record, indent=2) + "\n")

    save_record()
    if engine:
        package = output / ("Build" + host)
        command = [str(uat), "BuildPlugin", "-Plugin=" + str(stage / "GlooPrint.uplugin"),
                   "-Package=" + str(package), "-Rocket", "-StrictIncludes",
                   "-HostPlatforms=" + host, "-TargetPlatforms=" + host]
        record["build"] = {"status": "running", "platform": host, "engine": version, "command": command}
        save_record()
        print("Running installed-engine BuildPlugin; log: " + str(output / "Build.log"), flush=True)
        with (output / "Build.log").open("w") as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=False)
        record["build"]["exit_code"] = result.returncode
        record["build"]["status"] = "passed" if result.returncode == 0 else "failed"
        save_record()
        if result.returncode:
            raise RuntimeError("BuildPlugin failed. Read Build.log; no submission ZIP was produced.")
        built = json.loads((package / "GlooPrint.uplugin").read_text())
        if built.get("FabURL") != descriptor["FabURL"]:
            # UE 5.8 UAT rewrites only recognized fields and drops FabURL.
            # Keep Fab's required metadata in the local test installation too.
            built["FabURL"] = descriptor["FabURL"]
            (package / "GlooPrint.uplugin").write_text(json.dumps(built, indent=2) + "\n")
            record["build"]["descriptor_note"] = "Restored FabURL after UAT descriptor rewriting."
            save_record()
        if not (package / "Documentation/UserGuide.txt").is_file():
            raise RuntimeError("BuildPlugin omitted the customer guide. Check FilterPlugin.ini.")

    archive = output / ("GlooPrint_" + descriptor["VersionName"].replace("-", "_") + "_UE5_8_Source.zip")
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for relative in sorted(manifest):
            bundle.write(output / "Source" / relative, relative)
    with zipfile.ZipFile(archive) as bundle:
        if bundle.testzip() is not None or sorted(bundle.namelist()) != sorted(manifest):
            raise RuntimeError("Archive integrity or inventory mismatch.")
        for relative, digest in manifest.items():
            if hashlib.sha256(bundle.read(relative)).hexdigest() != digest:
                raise RuntimeError("Archive source hash mismatch: " + relative)
    record["archive"] = {"file": archive.name, "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                         "bytes": archive.stat().st_size}
    save_record()
    print("Source archive: " + str(archive))
    print("Release evidence: " + str(record_path))
    print("Windows testing and Fab review are still required before publication.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="New output directory (never overwritten)")
    parser.add_argument("--engine-root", type=Path, help="Installed UE_5.8 root; runs a strict native packaging build")
    arguments = parser.parse_args()
    try:
        prepare(arguments)
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        print("Release preparation failed: " + str(error), file=sys.stderr)
        sys.exit(1)
