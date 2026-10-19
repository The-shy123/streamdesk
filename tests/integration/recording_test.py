"""Real local capture/replay, interrupted files, and acknowledged playback controls."""
import argparse
import json
import os
from pathlib import Path
import selectors
import resource
import signal
import socket
import subprocess
import sys
import tempfile

from tcp_test import RunningServer, check, frame, message, ROOT


def replay(executable, path):
    return subprocess.run([executable, str(path)], capture_output=True, text=True, timeout=15)


def captured(server_exe, replay_exe, root, count):
    path = root / f"capture-{count}.sdr"
    verification = root / f"capture-{count}.jsonl"
    server = RunningServer(server_exe, ["--record", str(path), "--record-count", str(count),
                                        "--output", str(verification)])
    try:
        source = subprocess.run([sys.executable, str(ROOT / "tools/numeric_source.py"),
                                 "--port", str(server.port), "--count", str(count), "--fragment", "31"],
                                capture_output=True, text=True, timeout=15)
        check(source.returncode == 0, source.stderr)
        code, stats, _, error = server.result()
        check(code == 0 and stats["status"] == "completed" and stats["processed"] == count, error)
        check(path.exists() and not Path(str(path)+".partial").exists(), "completed publication")
        expected = [message(i) for i in range(count)]
        check([json.loads(line) for line in verification.read_text().splitlines()] == expected,
              "live verification fields/order")
        lines = path.read_bytes().splitlines(keepends=True)
        header, footer = json.loads(lines[0]), json.loads(lines[-1])
        check(header == {"format": "streamdesk.numeric", "version": 1, "expected_count": count,
                         "source_id": "sim", "stream_id": "values", "session_id": "fixed-run",
                         "first_seq": "0"}, "recording metadata")
        check([json.loads(line) for line in lines[1:-1]] == expected, "saved fields/order")
        checksum = 14695981039346656037
        for byte in b"".join(lines[:-1]):
            checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64)-1)
        check(footer == {"recording_end": "completed", "count": count, "last_seq": str(count-1),
                         "checksum_fnv1a64": f"{checksum:016x}"}, "independent footer/checksum")
        result = replay(replay_exe, path)
        check(result.returncode == 0, result.stderr)
        actual = [json.loads(line[6:]) for line in result.stdout.splitlines() if line.startswith("VALUE ")]
        check(actual == expected, "replay all fields/order equal source")
        summary = json.loads(next(line[7:] for line in result.stdout.splitlines() if line.startswith("REPLAY ")))
        check(summary == {"status": "completed", "count": count, "total": count,
                          "sum": count*(count-1)/4}, "replay summary")
        print(f"PASS capture/replay {count}: source/live/saved/replayed fields, order and independent checksum")
        return path
    finally:
        server.cleanup()


def incomplete(server_exe, replay_exe, root, label, wire, expected=3, stop=False, crash=False):
    path = root / (label+".sdr")
    server = RunningServer(server_exe, ["--record", str(path), "--record-count", str(expected)])
    try:
        with socket.create_connection(("127.0.0.1", server.port), timeout=3) as connection:
            check(server.line().strip() == "CONNECTED", "connection acknowledged")
            connection.sendall(wire)
            if stop or crash:
                check(server.line().startswith("VALUE 0 "), "first record durably flushed to stdio/OS before signal")
                server.process.send_signal(signal.SIGKILL if crash else signal.SIGINT)
            else:
                connection.shutdown(socket.SHUT_WR)
            if crash:
                server.process.communicate(timeout=5)
                check(server.process.returncode == -signal.SIGKILL, "test process crash")
            else:
                code, stats, _, _ = server.result()
                check((stop and code == 0 and stats["status"] == "stopped") or
                      (not stop and code != 0 and stats["status"] == "failed"), "incomplete recording status")
        partial = Path(str(path)+".partial")
        check(not path.exists() and partial.exists(), "incomplete file never published")
        rejected = replay(replay_exe, partial)
        check(rejected.returncode != 0 and "VALUE " not in rejected.stdout, "partial replay refused")
        before = partial.read_bytes()
        restart = subprocess.run([server_exe, "--record", str(path), "--record-count", str(expected)],
                                 capture_output=True, text=True, timeout=5)
        check(restart.returncode != 0 and partial.read_bytes() == before, "restart preserves interrupted data")
        print(f"PASS {label}: incomplete retained, replay refused, restart does not overwrite")
    finally:
        server.cleanup()


class Interactive:
    def __init__(self, executable, path):
        self.process = subprocess.Popen([executable, str(path), "--interactive", "--interval-ms", "1000"],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.buffer = b""
        self.lines = []

    def line(self):
        while b"\n" not in self.buffer:
            with selectors.DefaultSelector() as selector:
                selector.register(self.process.stdout, selectors.EVENT_READ)
                check(bool(selector.select(5)), "interactive output timeout")
            chunk = os.read(self.process.stdout.fileno(), 4096)
            check(bool(chunk), "unexpected interactive EOF")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        text = line.decode()
        self.lines.append(text)
        return text

    def send(self, command):
        self.process.stdin.write((command+"\n").encode()); self.process.stdin.flush()

    def finish(self):
        check(self.process.wait(timeout=5) == 0, "interactive exit")

    def cleanup(self):
        if self.process.poll() is None:
            self.process.kill(); self.process.wait(timeout=5)


def controls(executable, path):
    session = Interactive(executable, path)
    try:
        check(session.line() == "REPLAY_READY 10000", "ready")
        check(session.line() == "STATE paused 0/10000", "initial pause")
        session.send("next")
        check(json.loads(session.line()[6:]) == message(0), "single step")
        check(session.line() == "STATE paused 1/10000", "single step remains paused")
        session.send("play")
        check(session.line() == "STATE playing 1/10000", "play acknowledged")
        check(json.loads(session.line()[6:]) == message(1), "next ordered record")
        session.send("pause")
        check(session.line() == "STATE paused 2/10000", "pause acknowledged")
        session.send("status\nstatus")
        check(session.line() == session.line() == "STATE paused 2/10000", "paused controls do not read ahead")
        session.send("resume")
        check(session.line() == "STATE playing 2/10000", "resume acknowledged")
        check(json.loads(session.line()[6:]) == message(2), "resume at current position")
        session.send("pause\nstop")
        check(session.line() == "STATE paused 3/10000", "pause before stop")
        check(session.line() == "STATE stopped 3/10000", "stop acknowledged")
        summary = json.loads(session.line()[7:])
        check(summary == {"status": "stopped", "count": 3, "total": 10000, "sum": 1.5}, "stopped not completed")
        session.finish()
    finally:
        session.cleanup()
    for signal_stop in [False, True]:
        session = Interactive(executable, path)
        try:
            session.line(); session.line()
            if signal_stop: session.process.send_signal(signal.SIGINT)
            else: session.process.stdin.close()
            summary = json.loads(session.line()[7:])
            check(summary["status"] == "stopped" and summary["count"] == 0, "EOF/signal cancels while paused")
            session.finish()
        finally:
            session.cleanup()
    print("PASS playback controls: step, play, pause, progress, resume, stop, stdin EOF and SIGINT")


def write_failure(server_exe, root):
    # Restrict just this test child. No disk filling or system-wide limit changes.
    def limit_file_size():
        resource.setrlimit(resource.RLIMIT_FSIZE, (512, 512))
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)

    class LimitedServer(RunningServer):
        def __init__(self, path):
            self.process = subprocess.Popen([server_exe, "--port", "0", "--record", str(path),
                                             "--record-count", "10"], stdout=subprocess.PIPE,
                                            stderr=subprocess.PIPE, text=True, preexec_fn=limit_file_size)
            self.prefix = self.line()
            parts = self.prefix.split()
            check(parts[:2] == ["READY", "127.0.0.1"], "limited server readiness")
            self.port = int(parts[2])

    path = root/"write-failure.sdr"
    server = LimitedServer(path)
    try:
        with socket.create_connection(("127.0.0.1", server.port), timeout=3) as connection:
            connection.sendall(b"".join(frame(message(i)) for i in range(10)))
            connection.shutdown(socket.SHUT_WR)
        code, stats, _, diagnostic = server.result()
        check(code != 0 and stats["failed"] > 0 and stats["status"] == "failed", "write failure visible")
        check(not path.exists() and Path(str(path)+".partial").stat().st_size <= 512,
              "OS write failure never publishes")
        check("worker failure: recording write failed" in diagnostic, "first write error survives later queued failures")
        print("PASS OS write failure: per-child file limit, failed status, bounded partial, no publication")
    finally:
        server.cleanup()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("server"); parser.add_argument("replay")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="streamdesk-record-integration-") as temporary:
        root = Path(temporary)
        one = captured(args.server, args.replay, root, 1)
        full = captured(args.server, args.replay, root, 10000)
        incomplete(args.server, args.replay, root, "early-eof", frame(message(0)))
        incomplete(args.server, args.replay, root, "partial-frame", frame(message(0))[:-2])
        incomplete(args.server, args.replay, root, "extra-record", frame(message(0))+frame(message(1)), expected=1)
        incomplete(args.server, args.replay, root, "explicit-stop", frame(message(0)), stop=True)
        incomplete(args.server, args.replay, root, "process-crash", frame(message(0)), crash=True)
        write_failure(args.server, root)
        raw = one.read_bytes()
        for index, damaged in enumerate([raw[:-1], raw[:raw.rfind(b'{')], raw+b"junk\n",
                                          raw.replace(b'"version":1', b'"version":2'),
                                          raw.replace(b'"value":0.0', b'"value":2.0'),
                                          raw.replace(b'\n', b'\x00junk\n', 1)]):
            bad = root/f"corrupt-{index}.sdr"; bad.write_bytes(damaged)
            result = replay(args.replay, bad)
            check(result.returncode != 0 and "VALUE " not in result.stdout, "corrupt file must fail before output")
        print("PASS corrupt files: truncation/footer/trailing/version/value/checksum/NUL rejected before output")
        controls(args.replay, full)
        step = subprocess.run([args.replay, str(one), "--interactive"], input="next\n", text=True,
                              capture_output=True, timeout=5)
        check(step.returncode == 0 and '"status":"completed"' in step.stdout, "single step completion")
        conflict = root/"conflict.sdr"
        invalid = subprocess.run([args.server, "--record", str(conflict), "--record-count", "1",
                                  "--output", str(conflict)+".partial"], capture_output=True, text=True, timeout=5)
        check(invalid.returncode != 0 and not Path(str(conflict)+".partial").exists(), "path alias rejected before writes")
        print("PASS recording integration suite")


if __name__ == "__main__":
    main()
