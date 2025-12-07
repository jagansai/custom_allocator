# Custom Allocator / FIX Demo

This repo demonstrates a simple custom arena allocator in C++, compares it against regular heap allocation, and exposes both via a small FIX message benchmark and a TCP server.

## 1. ArenaAllocator

The arena allocator is implemented in [include/fix_demo.h](include/fix_demo.h) as `fix_demo::ArenaAllocator`:

- Allocates a single contiguous buffer up front.
- Hands out objects via `create<T>(...)` and `createOwned<T>(...)`.
- Uses simple bump-pointer allocation (`alignUp`) with no per-object `free`.
- Supports resetting to a previous mark for very fast bulk deallocation.

This is ideal for workloads where many objects share the same lifetime, such as parsing a FIX message into a `NewOrderSingle` and then discarding it.

## 2. RAII Wrappers

Two small RAII helpers in [include/fix_demo.h](include/fix_demo.h) make the arena easy and safe to use:

- `fix_demo::ArenaScope`
  - Captures the arena's current `used()` size on construction.
  - On destruction, resets the arena back to that mark.
  - Lets you treat a block of code like a temporary allocation region.

- `fix_demo::ArenaOwned<T>`
  - Wraps a pointer allocated from the arena.
  - Destroys the object on reset/destruction (but does not free memory from the arena).
  - Provides `get()`, `operator->`, `operator*`, and `release()`.

The FIX message objects are created via:

- `fix_demo::createOrderOnArena(ArenaAllocator&, std::string_view)`
- `fix_demo::createOrderOnHeap(std::string_view)`

and converted to JSON with `fix_demo::toJson(const NewOrderSingle&)`, which also includes an `arrivalTime` field for server-mode logging.

## 3. Test Setup

The test harness lives primarily in [src/run_modes.cpp](src/run_modes.cpp) and [src/main.cpp](src/main.cpp):

- **Standalone mode**
  - Reads FIX messages from `data/fix_messages.txt`.
  - Parses and converts the first message to JSON using both arena and heap and writes:
    - `arena_sample.json`, `heap_sample.json` in the output directory.
  - Benchmarks processing all messages with:
    - Arena allocation (`createOrderOnArena`).
    - Heap allocation (`createOrderOnHeap`).
  - Writes full-batch JSON outputs:
    - `arena_batch.json`, `heap_batch.json`.

- **Server mode**
  - Reads endpoints from a Java-style properties file, e.g. [config/app.properties](config/app.properties):
    - `arena.session.host`, `arena.session.port`
    - `heap.session.host`, `heap.session.port`
  - Starts two TCP listener threads:
    - **Arena session**: parses messages using the arena allocator.
    - **Heap session**: parses messages using regular `new`.
  - For each session it writes:
    - JSON stream: `arena_stream.json` / `heap_stream.json` (one JSON object per line, including `arrivalTime`).
    - Raw FIX log: `arena_raw.log` / `heap_raw.log` with lines like:
      - `hh:mm:ss.sss Incoming : <fix message>`
      - `hh:mm:ss.sss Heartbeat : processed N messages, waiting for new messages`
  - Heartbeats also periodically flush both JSON and raw logs.

## 4. How to Build

This project uses CMake. From the repo root:

```powershell
# Configure (one-time)
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build-release --config Release
```

The resulting binary will be in:

- `build-release/Release/fix_allocator_demo.exe` (on Windows/MSVC)

## 5. How to Run the C++ Binary

The binary has two modes, selected via `--mode`.

### Standalone mode

Process and benchmark FIX messages from a file:

```powershell
# From repo root
build-release/Release/fix_allocator_demo.exe `
  --mode standalone `
  --file data/fix_messages.txt `
  --output-dir logs/allocator
```

- `--file` defaults to `data/fix_messages.txt`.
- `--output-dir` defaults to `logs/allocator`.
Outputs (in the chosen `--output-dir`):

- `arena_sample.json`, `heap_sample.json`
- `arena_batch.json`, `heap_batch.json`

### Server mode

Start both arena and heap TCP listener sessions:

```powershell
# From repo root
build-release/Release/fix_allocator_demo.exe `
  --mode server `
  --server-config config/app.properties `
  --output-dir logs/allocator
```

- `--server-config` defaults to `config/app.properties`.
- `--output-dir` is where the JSON and raw logs will be written.

Example `config/app.properties`:

```properties
arena.session.host=localhost
arena.session.port=8080

heap.session.host=localhost
heap.session.port=8081
```

## 6. How to Set Up Data (Python)

A helper script generates a synthetic FIX message file.

From the repo root:

```powershell
python scripts/generate_fix_messages.py
```

By default this creates/overwrites:

- `data/fix_messages.txt`

which is then used by both the C++ binary (standalone mode) and the sender script (see below).

## 7. How to Inject Messages

The script [scripts/send_fix_messages.py](scripts/send_fix_messages.py) sends FIX messages over TCP to the arena and heap sessions and then summarizes processing time.

Make sure the C++ server is already running in `--mode server`, then in another terminal from the repo root run:

```powershell
python scripts/send_fix_messages.py `
  --config config/app.properties `
  --file data/fix_messages.txt `
  --log-dir logs/allocator
```

What it does:

- Reads FIX messages from the `--file` (default `data/fix_messages.txt`).
- Parses endpoints from the `--config` properties file.
- Connects to the arena session first and sends all messages.
- Then connects to the heap session and sends the same messages.
- Waits until the raw logs report that all messages were processed (via heartbeat lines).
- Reads `arrivalTime` from the JSON stream logs to compute and print:
  - Total duration for arena processing.
  - Total duration for heap processing.

This provides a simple, reproducible way to compare arena vs heap allocation under a realistic message load.

## 8. Benchmark Results (2025-12-07)

Using the server mode with the multi-run harness (`scripts/run.ps1` on Windows or `scripts/run.sh` on Linux), and a larger FIX message (additional fields such as account, order type, time-in-force, trader, firm, text, security description, and currency), a 10-run experiment with 100,000 messages per run produced the following summary:

| Run | Messages | FirstSent | ArenaTimeS | HeapTimeS | Winner |
|-----|----------|-----------|------------|-----------|--------|
| 1   | 100000   | Arena     | 0.64       | 0.62      | Heap   |
| 2   | 100000   | Arena     | 0.83       | 0.83      | Heap   |
| 3   | 100000   | Arena     | 0.62       | 0.64      | Arena  |
| 4   | 100000   | Heap      | 1.83       | 0.62      | Heap   |
| 5   | 100000   | Heap      | 0.58       | 0.61      | Arena  |
| 6   | 100000   | Heap      | 0.63       | 0.65      | Arena  |
| 7   | 100000   | Heap      | 0.66       | 0.60      | Heap   |
| 8   | 100000   | Arena     | 0.72       | 0.60      | Heap   |
| 9   | 100000   | Arena     | 0.57       | 0.57      | Heap   |
| 10  | 100000   | Arena     | 4.35       | 0.60      | Heap   |

Observation: for this workload and implementation, the custom arena allocator does not show a performance advantage over regular heap allocation; in these runs, heap is often slightly faster, and the differences are small enough that other costs (parsing, JSON serialization, logging, scheduling) likely dominate. A future improvement may compare the actual JSON payloads (excluding the `arrivalTime` field) to verify that both allocators produce identical message content.
