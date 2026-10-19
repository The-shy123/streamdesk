"""Run and verify the existing local numeric pipeline with one command."""
import argparse
import json
import shlex
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from numeric_source import message

ROOT = Path(__file__).resolve().parents[1]


def require(condition, reason):
    if not condition:
        raise RuntimeError(reason)


def run_logged(argv, log):
    with log.open("w") as output:
        result = subprocess.run(argv, stdout=output, stderr=subprocess.STDOUT,
                                text=True, timeout=15)
    require(result.returncode == 0, f"command failed ({result.returncode}); see {log}")


def wait_ready(process, log):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        require(process.poll() is None, "server exited before READY; see server logs")
        text = log.read_text()
        if "\n" in text:
            lines = text.splitlines()
            parts = lines[0].split()
            require(len(parts) == 3 and parts[:2] == ["READY", "127.0.0.1"],
                    "unexpected server readiness output")
            port = int(parts[2])
            require(1 <= port <= 65535, "invalid server port")
            return port
        # Poll an explicit readiness signal; elapsed time never means success.
        time.sleep(0.02)
    raise RuntimeError("server readiness timed out")


def demonstrate(build, record=False):
    server = build / "apps/server/streamdesk_server"
    inspector = build / "streamdesk_inspect"
    replayer = build / "streamdesk_replay"
    require(server.is_file() and inspector.is_file(),
            f"build server and inspector first; see README.md (build directory: {build})")
    require(not record or replayer.is_file(), "build streamdesk_replay first; see README.md")
    directory = Path(tempfile.mkdtemp(prefix="demo-", dir=ROOT / "build"))
    print(f"Evidence: {directory}", flush=True)
    received = directory / "numeric.jsonl"
    recording = directory / "numeric.sdr"
    server_log = directory / "server.log"
    process = None
    try:
        with server_log.open("w") as output, (directory / "server-error.log").open("w") as errors:
            extra = ["--record", str(recording), "--record-count", "1000"] if record else []
            process = subprocess.Popen([str(server), "--port", "0", "--deadline-seconds", "30",
                                        "--output", str(received), *extra], stdout=output, stderr=errors)
            port = wait_ready(process, server_log)
            run_logged([sys.executable, str(ROOT / "tools/numeric_source.py"),
                        "--port", str(port), "--count", "1000", "--fragment", "17"],
                       directory / "source.log")
            require(process.wait(timeout=15) == 0, "server failed; see server logs")
        summaries = [json.loads(line[len("SUMMARY "):]) for line in server_log.read_text().splitlines()
                     if line.startswith("SUMMARY ")]
        require(len(summaries) == 1, "expected exactly one server summary")
        stats = summaries[0]
        require(stats["status"] == "completed" and stats["accepted"] == stats["processed"] == 1000
                and stats["failed"] == stats["invalid"] == stats["discarded_pending"] == 0
                and stats["sum"] == 249750, "server summary mismatch")
        actual = [json.loads(line) for line in received.read_text().splitlines()]
        require(actual == [message(seq) for seq in range(1000)], "record field/order mismatch")
        run_logged([str(inspector), str(received)], directory / "inspect.log")
        inspected = [json.loads(line[len("INSPECT "):])
                     for line in (directory / "inspect.log").read_text().splitlines()
                     if line.startswith("INSPECT ")]
        require(inspected == [{"count": 1000, "sum": 249750}], "inspector summary mismatch")
        result = {"status": "passed", "records": 1000, "sum": 249750,
                  "all_fields_and_order_match": True, "server": stats}
        if record:
            run_logged([str(replayer), str(recording)], directory / "replay.log")
            replay_lines = (directory / "replay.log").read_text().splitlines()
            replayed = [json.loads(line[6:]) for line in replay_lines if line.startswith("VALUE ")]
            summaries = [json.loads(line[7:]) for line in replay_lines if line.startswith("REPLAY ")]
            require(replayed == actual, "recorded replay differs from live data")
            require(summaries == [{"status": "completed", "count": 1000, "total": 1000, "sum": 249750}],
                    "recorded replay summary mismatch")
            result.update(recording=str(recording), replay_matches_live=True)
            print(f"Recording: {recording}")
            print("PASS: saved recording replay matches every live record.")
            browser = build / "streamdesk_records"
            if browser.is_file():
                print("Browse saved records: " + shlex.join([str(browser), str(ROOT / "build"), "--interactive"]))
        (directory / "verification.json").write_text(json.dumps(result, indent=2) + "\n")
        print("PASS: 1000 records; all fields/order match; sum=249750; core inspector passed.")
        print("This is a functional check, not a performance measurement.")
    except BaseException as error:
        # Clean up only the child this run owns, retaining all evidence on failure.
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        (directory / "verification.json").write_text(
            json.dumps({"status": "failed", "error": str(error) or type(error).__name__}, indent=2) + "\n")
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/cmake-debug")
    parser.add_argument("--record", action="store_true", help="also save a completed recording and verify replay")
    args = parser.parse_args()
    try:
        demonstrate(args.build_dir.resolve(), args.record)
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.TimeoutExpired) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("ERROR: demonstration interrupted", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
