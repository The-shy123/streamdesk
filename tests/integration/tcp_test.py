"""Bounded tests of our own loopback server; no external services."""
import argparse
import json
from pathlib import Path
import selectors
import signal
import socket
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from numeric_source import frame, message


def check(condition, description):
    if not condition:
        raise RuntimeError(description)


class RunningServer:
    def __init__(self, executable, extra=()):
        self.process = subprocess.Popen([executable, "--port", "0", *extra],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.prefix = self.line()
        parts = self.prefix.split()
        check(parts[:2] == ["READY", "127.0.0.1"], "server did not become ready: " + self.prefix)
        self.port = int(parts[2])

    def line(self):
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout, selectors.EVENT_READ)
            if not selector.select(10):
                self.process.kill()
                self.process.communicate()
                raise RuntimeError("server output timeout")
        return self.process.stdout.readline()

    def result(self):
        stdout, stderr = self.process.communicate(timeout=10)
        lines = (self.prefix + stdout).splitlines()
        summaries = [line.removeprefix("SUMMARY ") for line in lines if line.startswith("SUMMARY ")]
        check(len(summaries) == 1, f"missing summary: {lines!r} {stderr!r}")
        return self.process.returncode, json.loads(summaries[0]), lines, stderr

    def cleanup(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.communicate()
            raise RuntimeError("test left server alive; terminated after failure")


def normal(executable, inspector, count):
    with tempfile.TemporaryDirectory(prefix="streamdesk-test-") as directory:
        output = Path(directory) / "received.jsonl"
        server = RunningServer(executable, ["--output", str(output), "--queue-count", "1"])
        try:
            source = subprocess.run([sys.executable, str(ROOT / "tools/numeric_source.py"),
                                     "--port", str(server.port), "--count", str(count), "--fragment", "13"],
                                    capture_output=True, text=True, timeout=10)
            check(source.returncode == 0, source.stderr)
            code, stats, lines, stderr = server.result()
            check(code == 0 and stats["status"] == "completed", f"normal status: {stats} {stderr}")
            actual = [json.loads(line) for line in output.read_text().splitlines()]
            expected = [message(seq) for seq in range(count)]
            check(actual == expected, "field-by-field content/order mismatch")
            check(stats["processed"] == stats["accepted"] == count and stats["failed"] == stats["invalid"] == 0, "normal counters")
            check(stats["sum"] == sum(seq * 0.5 for seq in range(count)), "sum mismatch")
            check(stats["peak_count"] <= 1 and stats["peak_bytes"] <= 262144, "capacity overrun")
            previews = [line for line in lines if line.startswith("VALUE ")]
            check(len(previews) == min(count, 20), "bounded preview count")
            viewed = subprocess.run([inspector, str(output)], capture_output=True, text=True, timeout=5)
            check(viewed.returncode == 0, viewed.stderr)
            summary = json.loads(next(line[8:] for line in viewed.stdout.splitlines() if line.startswith("INSPECT ")))
            check(summary == {"count": count, "sum": stats["sum"]}, "CLI inspection mismatch")
            print(f"PASS normal {count}: all fields/order/sum, queue_count=1, preview={len(previews)}, core CLI inspected")
        finally:
            server.cleanup()


def invalid(executable, wire, label, extra=()):
    server = RunningServer(executable, extra)
    try:
        with socket.create_connection(("127.0.0.1", server.port), timeout=3) as connection:
            connection.sendall(wire)
            connection.shutdown(socket.SHUT_WR)
        code, stats, _, stderr = server.result()
        check(code != 0 and stats["status"] == "failed", f"{label}: should fail, {stats} {stderr}")
        check(stats["processed"] + stats["failed"] == stats["accepted"], "accepted work must drain")
        print(f"PASS {label}: rejected, exit={code}, diagnostic={stderr.strip()}")
    finally:
        server.cleanup()


def stop(executable):
    server = RunningServer(executable)
    try:
        with socket.create_connection(("127.0.0.1", server.port), timeout=3) as connection:
            # Wait for connection acknowledgement before sending the task.
            check(server.line().strip() == "CONNECTED", "connection acknowledgement")
            connection.sendall(frame(message(0)))
            # This output is a synchronization point, not a guessed sleep.
            check(server.line().startswith("VALUE 0 "), "task did not run")
            server.process.send_signal(signal.SIGINT)
            code, stats, _, _ = server.result()
            check(code == 0 and stats["status"] == "stopped" and stats["accepted"] == stats["processed"] == 1, "stop must drain")
        print("PASS explicit stop: active connection closed, accepted work drained")
    finally:
        server.cleanup()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("server")
    parser.add_argument("inspector")
    args = parser.parse_args()
    normal(args.server, args.inspector, 5)
    normal(args.server, args.inspector, 1000)
    bad = message(0); bad["seq"] = 0
    invalid(args.server, frame(bad), "invalid JSON field")
    invalid(args.server, b"\0\0", "disconnect in header")
    invalid(args.server, frame(message(0))[:-3], "disconnect in body")
    invalid(args.server, struct.pack("!I", 65537), "oversized length")
    invalid(args.server, frame(message(0)) + frame(message(2)), "sequence gap")
    invalid(args.server, frame(message(0)) + frame(message(0)), "duplicate sequence")
    invalid(args.server, frame(message(0)), "capacity too small", ["--queue-bytes", "1"])
    invalid(args.server, struct.pack("!I", 1) + b"{", "invalid JSON syntax")
    invalid(args.server, struct.pack("!I", 0), "zero frame length")
    nul_body = json.dumps(message(0)).encode() + b"\x00junk"
    invalid(args.server, struct.pack("!I", len(nul_body)) + nul_body, "raw NUL trailing data")
    changed = message(1); changed["session_id"] = "different-run"
    invalid(args.server, frame(message(0)) + frame(changed), "session switch")
    invalid(args.server, frame(message(0)) + frame(message(1)), "record limit", ["--max-records", "1"])
    huge0, huge1 = message(0), message(1)
    huge0["payload"]["value"] = huge1["payload"]["value"] = 1e308
    invalid(args.server, frame(huge0) + frame(huge1), "observable aggregate failure")
    stop(args.server)
    server = RunningServer(args.server, ["--deadline-seconds", "1"])
    try:
        code, stats, _, _ = server.result()
        check(code != 0 and stats["status"] == "failed", "overall deadline should fail")
        print("PASS idle listener deadline")
    finally:
        server.cleanup()
    server = RunningServer(args.server, ["--deadline-seconds", "10"])
    try:
        with socket.create_connection(("127.0.0.1", server.port), timeout=3) as connection:
            connection.sendall(b"\x00")
            code, stats, _, stderr = server.result()
            check(code != 0 and stats["status"] == "failed" and "partial-message timeout" in stderr, "partial input must time out")
        print("PASS incomplete message idle timeout")
    finally:
        server.cleanup()
    with tempfile.TemporaryDirectory(prefix="streamdesk-inspect-") as directory:
        path = Path(directory) / "bad.jsonl"
        for bad in ["", "{\n", "x" * 65537 + "\n", json.dumps(message(0)) + "\x00junk\n"]:
            path.write_text(bad)
            result = subprocess.run([args.inspector, str(path)], capture_output=True, text=True, timeout=5)
            check(result.returncode != 0, "inspector accepted corrupt/overlong input")
        path.write_text(json.dumps(message(0)))
        result = subprocess.run([args.inspector, str(path)], capture_output=True, text=True, timeout=5)
        check(result.returncode == 0 and '"count":1' in result.stdout, "final line without newline must retain full length")
        path.write_text("preserve me")
        result = subprocess.run([args.server, "--output", str(path)], capture_output=True, text=True, timeout=5)
        check(result.returncode != 0 and path.read_text() == "preserve me", "existing output must be preserved")
    print("PASS TCP/CLI suite: 18 network scenarios + 6 file checks; loopback only")


if __name__ == "__main__":
    main()
