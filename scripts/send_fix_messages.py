import argparse
import json
import re
import socket
import time
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Tuple


def parse_properties(path: Path) -> Dict[str, str]:
    props: Dict[str, str] = {}
    if not path.is_file():
        raise FileNotFoundError(f"config file not found: {path}")

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        props[key.strip()] = value.strip()
    return props


def load_messages(path: Path) -> List[str]:
    if not path.is_file():
        raise FileNotFoundError(f"messages file not found: {path}")
    lines = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line:
            continue
        lines.append(line)
    return lines


def send_messages(host: str, port: int, messages: List[str], label: str) -> None:
    addr: Tuple[str, int] = (host or "127.0.0.1", port)
    print(f"Connecting to {label} session at {addr[0]}:{addr[1]} ...")
    with socket.create_connection(addr) as sock:
        for msg in messages:
            data = (msg.rstrip("\r\n") + "\n").encode("ascii", errors="replace")
            sock.sendall(data)
    print(f"Sent {len(messages)} messages to {label} session")


def wait_for_log_completion(log_path: Path, expected_count: int, label: str, timeout: float = 60.0) -> None:
    """Poll the raw log until we see a heartbeat confirming processing.

    Looks for lines like:
      "Heartbeat : processed N messages, waiting for new messages"
    and returns once N >= expected_count or when the timeout elapses.
    """

    pattern = re.compile(r"processed\s+(\d+)\s+messages,\s+waiting for new messages")
    start = time.time()
    last_seen = None

    print(f"Waiting for processing confirmation in {log_path} for {label} ...")

    while True:
        if log_path.is_file():
            text = log_path.read_text(encoding="utf-8", errors="ignore")
            for line in text.splitlines():
                match = pattern.search(line)
                if match:
                    count = int(match.group(1))
                    last_seen = count
                    if count >= expected_count:
                        print(f"{label}: heartbeat reports {count} messages processed")
                        return

        if time.time() - start > timeout:
            print(f"{label}: timeout waiting for {expected_count} messages (last seen={last_seen})")
            return

        time.sleep(0.5)


def compute_duration_from_stream(stream_path: Path) -> float | None:
    """Return duration in seconds between first and last arrivalTime in a JSON stream file."""

    if not stream_path.is_file():
        print(f"Stream file not found: {stream_path}")
        return None

    first_ts: datetime | None = None
    last_ts: datetime | None = None

    for line in stream_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        ts_str = obj.get("arrivalTime")
        if not ts_str:
            continue
        # Format is e.g. 2025-12-06T10:23:45.123456Z
        ts_norm = ts_str.replace("Z", "+00:00")
        try:
            ts = datetime.fromisoformat(ts_norm)
        except ValueError:
            continue
        if first_ts is None:
            first_ts = ts
        last_ts = ts

    if first_ts is None or last_ts is None:
        print(f"No arrivalTime timestamps found in {stream_path}")
        return None

    return (last_ts - first_ts).total_seconds()


def main() -> None:
    parser = argparse.ArgumentParser(description="Send FIX messages to arena and heap sessions")
    _ = parser.add_argument(
        "--config",
        dest="config",
        default="config/app.properties",
        help="Path to server config properties (default: config/app.properties)",
    )
    _ = parser.add_argument(
        "--file",
        dest="file",
        default="data/fix_messages.txt",
        help="Path to FIX messages file (default: data/fix_messages.txt)",
    )
    _ = parser.add_argument(
        "--log-dir",
        dest="log_dir",
        default="output",
        help=(
            "Directory where server writes logs (arena_raw.log, heap_raw.log, "
            "arena_stream.json, heap_stream.json). Default: output"
        ),
    )
    args = parser.parse_args()

    config_path = Path(args.config)
    messages_path = Path(args.file)
    log_dir = Path(args.log_dir)

    props = parse_properties(config_path)
    messages = load_messages(messages_path)
    if not messages:
        print(f"No messages found in {messages_path}")
        return

    try:
        arena_host = props.get("arena.session.host", "127.0.0.1")
        arena_port = int(props["arena.session.port"])
        heap_host = props.get("heap.session.host", "127.0.0.1")
        heap_port = int(props["heap.session.port"])
    except KeyError as exc:
        raise SystemExit(f"Missing required config key: {exc}") from exc

    # First push all messages to the arena session, then to the heap session.
    send_messages(arena_host, arena_port, messages, label="arena")
    send_messages(heap_host, heap_port, messages, label="heap")

    # Wait until both arena and heap have reported processing the full batch.
    arena_raw = log_dir / "arena_raw.log"
    heap_raw = log_dir / "heap_raw.log"
    expected = len(messages)
    wait_for_log_completion(arena_raw, expected, label="arena")
    wait_for_log_completion(heap_raw, expected, label="heap")

    # Compute timing based on arrivalTime in the JSON stream logs.
    arena_stream = log_dir / "arena_stream.json"
    heap_stream = log_dir / "heap_stream.json"

    arena_duration = compute_duration_from_stream(arena_stream)
    heap_duration = compute_duration_from_stream(heap_stream)

    if arena_duration is not None:
        print(f"Arena: {expected} messages over {arena_duration:.3f} seconds")
    if heap_duration is not None:
        print(f"Heap:  {expected} messages over {heap_duration:.3f} seconds")


if __name__ == "__main__":
    main()
