"""Read-only discovery/details and selection-to-replay with one shared command stream."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import tempfile

from recording_test import captured
from tcp_test import check, message


def entries(output):
    result = {}
    for line in output.splitlines():
        if line.startswith("ENTRY "):
            _, number, state, size, path = line.split(" ", 4)
            result[json.loads(path)] = (int(number), state, size)
    return result


class Browser:
    def __init__(self, executable, directory):
        self.process = subprocess.Popen([executable, str(directory), "--interactive", "--interval-ms", "0"],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.buffer = b""

    def line(self):
        while b"\n" not in self.buffer:
            with selectors.DefaultSelector() as selector:
                selector.register(self.process.stdout, selectors.EVENT_READ)
                check(bool(selector.select(5)), "browser output timeout")
            chunk = os.read(self.process.stdout.fileno(), 4096)
            check(bool(chunk), "unexpected browser EOF")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return line.decode()

    def ready(self):
        lines = []
        for _ in range(200):
            line = self.line(); lines.append(line)
            if line == "BROWSER_READY": return "\n".join(lines)
        raise RuntimeError("browser did not return to list")

    def send(self, commands):
        self.process.stdin.write((commands+"\n").encode()); self.process.stdin.flush()

    def close(self):
        if self.process.poll() is None:
            self.process.kill(); self.process.wait(timeout=5)


def snapshot(root):
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file() and not path.is_symlink()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("browser"); parser.add_argument("server"); parser.add_argument("replay")
    args = parser.parse_args()
    def run(directory, *options, commands=None):
        return subprocess.run([args.browser, str(directory), *options], input=commands,
                              capture_output=True, text=True, timeout=15)

    with tempfile.TemporaryDirectory(prefix="streamdesk-catalog-integration-") as temporary:
        base = Path(temporary)
        first = captured(args.server, args.replay, base, 6).read_bytes()
        second = captured(args.server, args.replay, base, 1).read_bytes()
        root = base/"保存的记录"; root.mkdir()
        a, b = "a first 中文.sdr", "b second.sdr"
        (root/a).write_bytes(first); (root/b).write_bytes(second)
        (root/"c bad.sdr").write_bytes(b"damaged")
        (root/"d.sdr.partial").write_bytes(first[:-100])
        odd = 'x\n\x1b[31m"quoted".sdr'
        (root/odd).write_bytes(second)
        (root/"nested").mkdir(); (root/"nested/child.sdr").write_bytes(second)
        (root/"linked.sdr").symlink_to(root/a)
        (root/"ignore.jsonl").write_text("unrelated")
        original = snapshot(root)
        listed = run(root)
        check(listed.returncode == 0, listed.stderr)
        mapping = entries(listed.stdout)
        check(len(mapping) == 7 and mapping[a][0] == 1 and mapping[b][0] == 2, "sorted list with depth one")
        check(mapping[a][1] == "unchecked" and mapping["d.sdr.partial"][1] == "incomplete" and
              mapping["linked.sdr"][1] == "unavailable", "honest discovery statuses")
        check(odd in mapping and "\x1b" not in listed.stdout and '\\u001b' in listed.stdout, "terminal control bytes escaped")
        shown = run(root, "--show", "1")
        check(shown.returncode == 0 and "COUNT 6" in shown.stdout and "SEQUENCE 0 5" in shown.stdout, shown.stderr)
        preview = [json.loads(line[8:]) for line in shown.stdout.splitlines() if line.startswith("PREVIEW ")]
        check(preview == [message(i) for i in range(5)], "details contain at most five exact examples")
        check("DETAIL 1 ready" in shown.stdout, "full validation before ready details")
        played = run(root, "--replay", "2", "--interval-ms", "0")
        check(played.returncode == 0 and '"status":"completed"' in played.stdout, "one-shot selection replay")
        for selected in ["0", "999", str(mapping["c bad.sdr"][0]), str(mapping["d.sdr.partial"][0]),
                         str(mapping["linked.sdr"][0])]:
            rejected = run(root, "--replay", selected, "--interval-ms", "0")
            check(rejected.returncode != 0 and "VALUE " not in rejected.stdout, "unplayable selection rejected before output")
        empty = base/"empty"; empty.mkdir()
        check("CATALOG 0 records" in run(empty).stdout, "empty list")
        check(run(base/"missing").returncode != 0, "missing directory")
        check("nested/child.sdr" not in entries(run(root,"--depth","0").stdout), "depth option")
        print("PASS catalog list/details: metadata, bounded preview, statuses, empty/missing, depth and escaped names")

        # All commands in one pipe: returning from playback must not discard browser commands.
        result = run(root, "--interactive", "--interval-ms", "0",
                     commands="show 1\nreplay 1\nnext\nstop\nshow 2\nreplay 2\nnext\nlist\nquit\n")
        check(result.returncode == 0, result.stderr)
        summaries = [json.loads(line[7:]) for line in result.stdout.splitlines() if line.startswith("REPLAY ")]
        check(summaries == [{"status": "stopped", "count": 1, "total": 6, "sum": 0},
                            {"status": "completed", "count": 1, "total": 1, "sum": 0}], "switch resets playback progress")
        check("DETAIL 2 ready" in result.stdout and result.stdout.count("RETURNED_TO_RECORDS") == 2,
              "commands after stop/completion are preserved")
        check("BROWSER_CLOSED" in result.stdout and snapshot(root) == original, "browse/preview/replay are read-only")
        print("PASS selection workflow: step/stop returns, second file completes, batched commands retained, files unchanged")

        browser = Browser(args.browser, root)
        try:
            browser.ready()
            browser.send("show 1"); check("DETAIL 1 ready" in browser.ready(), "initial verification")
            (root/a).write_bytes(b"changed after verification")
            browser.send("replay 1")
            response = browser.ready()
            check("ERROR " in response and "VALUE " not in response, "revalidate selected file after mutation")
            browser.send("list"); check(entries(browser.ready())[a][1] == "invalid", "invalid cached status updated")
            (root/a).write_bytes(first)
            (root/b).unlink()
            browser.send("show 2"); check("ERROR " in browser.ready(), "deleted selection error")
            browser.send("list"); check(entries(browser.ready())[b][1] == "unavailable", "deleted row retained with same ID")
            (root/"00-new.sdr").write_bytes(second)
            browser.send("list"); check("00-new.sdr" not in entries(browser.ready()), "no implicit renumbering")
            browser.send("refresh")
            fresh = entries(browser.ready())
            check(fresh["00-new.sdr"][0] == 1 and fresh[a][0] == 2 and b not in fresh, "refresh discovers/removes and renumbers")
            browser.send("show bad-id"); check("ERROR " in browser.ready(), "bad command recoverable")
            browser.send("show 2"); check("DETAIL 2 ready" in browser.ready(), "browser remains usable")
            browser.send("quit")
            check(browser.line() == "BROWSER_CLOSED" and browser.process.wait(timeout=5) == 0, "normal browser exit")
        finally:
            browser.close()
        print("PASS stale list: changed/deleted files rejected, stable IDs until refresh, recovery after command errors")

        for during_replay in [False, True]:
            browser = Browser(args.browser, root)
            try:
                browser.ready()
                if during_replay:
                    browser.send("replay 1")
                    check(browser.line().startswith("SELECTED "), "selection")
                    check(browser.line() == "REPLAY_READY 1", "playback ready")
                    check(browser.line() == "STATE paused 0/1", "paused playback")
                browser.process.send_signal(signal.SIGINT)
                check(browser.process.wait(timeout=5) == 0, "SIGINT exits browser and any playback")
            finally:
                browser.close()
        eof = run(root, "--interactive", commands="")
        check(eof.returncode == 0 and "BROWSER_CLOSED" in eof.stdout, "stdin EOF")
        final_line = run(root, "--interactive", commands="show 1")
        check(final_line.returncode == 0 and "DETAIL 1 ready" in final_line.stdout, "last command without newline")
        too_long = run(root, "--interactive", commands="x"*129+"\n")
        check(too_long.returncode != 0 and "command too long" in too_long.stderr, "bounded commands")
        print("PASS browser lifecycle: SIGINT in list/playback, EOF, final command, input length bound")
        print("PASS recording catalog integration suite")


if __name__ == "__main__":
    main()
