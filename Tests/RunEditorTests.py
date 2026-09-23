#!/usr/bin/env python3
# Copyright 2026 Ishtmeet Singh. All Rights Reserved.
"""Test a packaged plugin in a fresh UE 5.8 editor project, without symlinks.

Uses Python 3.10+ and its standard library. Run on a desktop with a working GPU;
the suite exercises real Slate windows, painting, input, and editor restarts.
"""

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys
import time


def read_report(path):
    report = json.loads(path.read_text(encoding="utf-8-sig"))
    tests = report.get("tests", [])
    if not tests:
        raise ValueError("Automation report contains no tests.")
    failures = [test.get("fullTestPath", test.get("testDisplayName", "unknown"))
                for test in tests if test.get("state") != "Success"]
    if report.get("failed", 0) or report.get("notRun", 0) or report.get("inProcess", 0):
        failures.append("Report contains failed, incomplete, or unexecuted tests")
    return {"status": "failed" if failures else "passed", "total": len(tests),
            "succeeded": report.get("succeeded", 0),
            "succeeded_with_warnings": report.get("succeededWithWarnings", 0),
            "failures": failures}


def run(args):
    root = Path(__file__).resolve().parents[1]
    output = args.output.expanduser().resolve()
    package = args.plugin.expanduser().resolve()
    engine = args.engine_root.expanduser().resolve()
    if output.exists():
        raise ValueError("Output already exists. Choose a new directory; results are never replaced.")
    if output == package or output.is_relative_to(package) or package.is_relative_to(output):
        raise ValueError("Test output and packaged plugin must be separate directories.")
    if output == root or output in root.parents or output.is_relative_to(root / "Source"):
        raise ValueError("Test output cannot replace or modify source inputs.")
    version = json.loads((engine / "Engine/Build/Build.version").read_text(encoding="utf-8-sig"))
    if (version["MajorVersion"], version["MinorVersion"]) != (5, 8):
        raise ValueError("Use an installed UE 5.8 engine.")
    host = {"Windows": "Win64", "Darwin": "Mac"}.get(platform.system())
    if not host:
        raise ValueError("Editor validation is supported on Windows and macOS only.")
    editor = engine / ("Engine/Binaries/Win64/UnrealEditor.exe" if host == "Win64" else
                       "Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor")
    if not editor.is_file():
        raise ValueError("UnrealEditor executable was not found.")
    descriptor = json.loads((package / "GlooPrint.uplugin").read_text(encoding="utf-8-sig"))
    if not (package / "Binaries" / host / "UnrealEditor.modules").is_file():
        raise ValueError("Plugin has no native editor build. Run PrepareRelease.py --engine-root first.")
    if args.timeout <= 0:
        raise ValueError("Timeout must be positive.")
    if not args.tests.startswith("GlooPrint") or any(c in args.tests for c in '\"\r\n,;'):
        raise ValueError("Tests must be a GlooPrint automation prefix.")

    output.mkdir(parents=True)
    project = output / "HostProject"
    project.mkdir()
    (project / "Content").mkdir()
    project_file = project / "GlooPrintHost.uproject"
    shutil.copy2(root / "Tests/HostProject/GlooPrintHost.uproject", project_file)
    shutil.copytree(package, project / "Plugins/GlooPrint",
                    ignore=shutil.ignore_patterns("Intermediate", "Saved", "HostProject"))
    command = [str(editor), str(project_file), "-unattended", "-nosplash", "-nosound",
               "-NoLiveCoding", "-stdout", "-FullStdOutLogOutput",
               # Keep both operations in one Automation invocation: separate
               # console invocations reinitialize UE's controller and reset the filter.
               # TestExit forces termination and skips module/UObject teardown.
               "-ExecCmds=Automation SetFilter " + args.test_filter + ";RunTests " + args.tests + ";SoftQuit",
               "-ReportExportPath=" + str(output / "Report"),
               "-abslog=" + str(output / "Editor.log")]
    command.extend(getattr(args, "editor_arg", None) or [])
    record = {"status": "running", "platform": host, "engine": version,
              "plugin_version": descriptor["VersionName"], "command": command,
              "test_filter": args.test_filter, "test_prefix": args.tests}
    record_path = output / "TestRecord.json"

    def save():
        record_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")

    save()
    started = time.monotonic()
    print("Running native editor tests; log: " + str(output / "Editor.log"), flush=True)
    with (output / "Console.log").open("w", encoding="utf-8") as log:
        options = {}
        if host == "Win64":
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 1 if getattr(args, "visible", False) else subprocess.SW_HIDE  # SW_SHOWNORMAL
            options["startupinfo"] = startup
        else:
            # A disk-persistence test launches a second editor. Keep it in our
            # process group so a timed-out Mac run cannot leave a reader behind.
            options["start_new_session"] = True
        process = None
        try:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, **options)
            record["exit_code"] = process.wait(timeout=args.timeout)
            record.update(read_report(output / "Report/index.json"))
            if record["exit_code"] != 0:
                record["status"] = "failed"
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            record.update(status="failed", error=str(error))
        except KeyboardInterrupt:
            record.update(status="failed", error="Interrupted by user")
        finally:
            if process is not None and process.poll() is None:
                if host == "Win64":
                    subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                                   stdout=log, stderr=subprocess.STDOUT, check=False)
                else:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass  # The editor finished between poll and cleanup.
                process.wait()
            record["elapsed_seconds"] = round(time.monotonic() - started, 2)
            save()
    print("Editor validation: " + record["status"] + "; evidence: " + str(record_path), flush=True)
    return 0 if record["status"] == "passed" else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-root", type=Path, required=True)
    parser.add_argument("--plugin", type=Path, required=True, help="BuildWin64 or BuildMac from PrepareRelease.py")
    parser.add_argument("--output", type=Path, required=True, help="New output directory")
    parser.add_argument("--tests", default="GlooPrint", help="Automation test prefix")
    parser.add_argument("--test-filter", choices=("Engine", "Perf"), default="Engine")
    parser.add_argument("--timeout", type=float, default=1800, help="Maximum editor runtime in seconds")
    parser.add_argument("--editor-arg", action="append", default=[], help="Extra editor argument; use --editor-arg=-Flag")
    parser.add_argument("--visible", action="store_true", help="Show the Windows editor host for tests requiring native foreground input")
    try:
        sys.exit(run(parser.parse_args()))
    except (OSError, ValueError, KeyError) as error:
        print("Editor validation failed: " + str(error), file=sys.stderr)
        sys.exit(1)
