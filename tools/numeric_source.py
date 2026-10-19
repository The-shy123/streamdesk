"""Deterministic, loopback-only numeric source. No remote scanning or reconnect."""
import argparse
import json
import socket
import struct


def message(seq):
    return {
        "schema_version": 1, "source_id": "sim", "stream_id": "values",
        "session_id": "fixed-run", "seq": str(seq), "kind": "numeric",
        "source_time": {"value": str(seq * 1000), "unit": "us"},
        "payload": {"name": "sample", "value": seq * 0.5, "unit": "u"},
    }


def frame(value):
    body = json.dumps(value, separators=(",", ":"), allow_nan=False).encode("utf-8")
    if not 0 < len(body) <= 65536:
        raise ValueError("body limit exceeded")
    return struct.pack("!I", len(body)) + body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--count", type=int, default=1000)
    parser.add_argument("--fragment", type=int, default=17)
    args = parser.parse_args()
    if not (1 <= args.port <= 65535 and 1 <= args.count <= 10000 and 1 <= args.fragment <= 65536):
        parser.error("port/count/fragment outside bounded range")
    wire = b"".join(frame(message(seq)) for seq in range(args.count))
    with socket.create_connection(("127.0.0.1", args.port), timeout=5) as connection:
        for offset in range(0, len(wire), args.fragment):
            connection.sendall(wire[offset:offset + args.fragment])
        connection.shutdown(socket.SHUT_WR)
    print(f"SENT {args.count} fixed numeric records")


if __name__ == "__main__":
    main()
