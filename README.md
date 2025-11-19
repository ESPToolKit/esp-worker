# ESPWorker

ESPWorker is a C++17 helper library for ESP32 projects that want FreeRTOS power without the boilerplate. It wraps task creation, joins, diagnostics, PSRAM stacks, and lifecycle events into a simple API that works with both Arduino-ESP32 and ESP-IDF.

## CI / Release / License
[![CI](https://github.com/ESPToolKit/esp-worker/actions/workflows/ci.yml/badge.svg)](https://github.com/ESPToolKit/esp-worker/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ESPToolKit/esp-worker?sort=semver)](https://github.com/ESPToolKit/esp-worker/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Features
- Works with FreeRTOS tasks while keeping `std::function`/lambda ergonomics.
- Joinable workers with runtime diagnostics (`JobDiag`) and cooperative destruction.
- Pull worker-pool metrics (`WorkerDiag`) including counts and runtime stats.
- Optional PSRAM stacks (`spawnExt`) for memory hungry jobs.
- Thread-safe event and error callbacks so firmware can log or react centrally.
- Configurable defaults and guardrails (max workers, priorities, affinities).

## Examples
Quick start:

```cpp
#include <ESPWorker.h>

ESPWorker worker;

void setup() {
    Serial.begin(115200);

    worker.init({
        .maxWorkers = 4,
        .defaultStackSize = 4096,
        .defaultPriority = 1,
        .defaultCoreId = tskNO_AFFINITY,
        .enableExternalStacks = true,
    });

    worker.onEvent([](WorkerEvent event) {
        Serial.printf("[worker] %s\n", worker.eventToString(event));
    });
    worker.onError([](WorkerError error) {
        Serial.printf("[worker][error] %s\n", worker.errorToString(error));
    });

    WorkerResult result = worker.spawn([]() {
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }, {
        .stackSize = 4096,
        .priority = 3,
        .name = "sensor-task",
    });

    if (result) {
        result.handler->wait(pdMS_TO_TICKS(1000));
    }
}
```

Need deterministic cleanup? Store the handler and call `destroy()` when shutting down:

```cpp
WorkerResult job = worker.spawn([](){ /* ... */ });
if (job) {
    // Stop the task cooperatively
    job.handler->destroy();
}
```

Check the runnable examples under `examples/`:
- `examples/basic_worker` – spawns workers, waits for completion, prints diagnostics.
- `examples/psram_stack` – uses `spawnExt` to place heavy stacks in PSRAM.

## Gotchas
- Always call `worker.init()` once before spawning tasks. Each ESPWorker instance controls its own limits.
- `spawn` creates persistent FreeRTOS tasks; remember to end the lambda (return) or `destroy()` the handler to reclaim slots.
- Errors such as `MaxWorkersReached` or `TaskCreateFailed` are reported in the returned `WorkerResult` _and_ via the error callback.
- PSRAM stacks require PSRAM to be enabled in your board configuration; fall back to internal RAM otherwise.

## API Reference
- `void init(const ESPWorker::Config& config)` – sets defaults (max workers, default stack/priority/core, PSRAM allowance).
- `WorkerResult spawn(TaskCallback cb, const WorkerConfig& config = {})` – create a worker. The returned handler provides `wait()` and `destroy()` helpers plus per-job diagnostics.
- `WorkerResult spawnExt(...)` – identical to `spawn` but forces PSRAM stacks when available.
- `size_t activeWorkers() const` / `void cleanupFinished()` – query or prune finished tasks.
- `WorkerDiag getDiag() const` – aggregated counts and runtime stats across the pool.
- `void onEvent(EventCallback cb)` / `void onError(ErrorCallback cb)` – receive lifecycle signals (`Created → Started → Completed/Destroyed`) and fatal issues.
- `const char* eventToString(...)` / `errorToString(...)` – convert enums to printable text for logging.

`WorkerConfig` (per job) and `ESPWorker::Config` (global defaults) expose priority, stack size, core affinity, external stack usage, and an optional name that shows up in diagnostics and watchdog dumps.

## Restrictions
- Intended for ESP32-class boards where FreeRTOS and PSRAM are available; other architectures are untested.
- Requires C++17 support (`-std=gnu++17`) and should not be called from ISR context.
- Each worker consumes RAM proportional to its stack; keep `maxWorkers` and per-job stacks aligned with your heap budget.

## Tests
A native host test suite is still being assembled. For now rely on the `examples/` sketches (build with PlatformIO or Arduino IDE) to verify integration, and consider adding regression tests when contributing changes.

## License
ESPWorker is released under the [MIT License](LICENSE.md).

## ESPToolKit
- Check out other libraries: <https://github.com/orgs/ESPToolKit/repositories>
- Hang out on Discord: <https://discord.gg/WG8sSqAy>
- Support the project: <https://ko-fi.com/esptoolkit>
