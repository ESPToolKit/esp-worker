# ESPWorker

[![CI](https://github.com/ESPToolKit/esp-worker/actions/workflows/ci.yml/badge.svg)](https://github.com/ESPToolKit/esp-worker/actions/workflows/ci.yml)
[![Release](https://github.com/ESPToolKit/esp-worker/actions/workflows/release.yml/badge.svg)](https://github.com/ESPToolKit/esp-worker/actions/workflows/release.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

ESPWorker is a C++17 helper library for ESP32 projects that want FreeRTOS power without the boilerplate. It wraps task creation, joins, diagnostics, PSRAM stacks, and lifecycle events into a simple API that works with both Arduino-ESP32 and ESP-IDF.

- Works with FreeRTOS tasks while keeping the familiar `std::function`/lambda ergonomics
- Joinable workers with runtime diagnostics (`WorkerDiag`) and cooperative destruction
- Optional PSRAM stacks (`spawnExt`) for memory hungry jobs
- Thread-safe event and error callbacks so firmware can log or react centrally
- Configurable defaults and guardrails (max workers, priorities, affinities)

---

## Installation

### PlatformIO

Add the dependency to your `platformio.ini`:

```ini
[env:esp32dev]
platform = espressif32
framework = arduino
lib_deps =
    ESPToolKit/ESPWorker
build_unflags = -std=gnu++11
build_flags = -std=gnu++17
```

### Arduino IDE / Arduino CLI

1. Download the latest release archive from the GitHub Releases page.
2. In the Arduino IDE choose **Sketch → Include Library → Add .ZIP Library...** and select the archive.  
   With Arduino CLI you can install the local archive via `arduino-cli lib install <path-to-zip>`.

---

## Quick Start

```cpp
#include <Arduino.h>
#include <ESPWorker.h>

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

    WorkerConfig config;
    config.name = "sensor-task";
    config.priority = 3;

    WorkerResult result = worker.spawn([]() {
        while (true) {
            // Work inside its own FreeRTOS task context
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }, config);

    if (!result) {
        Serial.printf("Failed to spawn worker: %s\n", worker.errorToString(result.error));
        return;
    }

    // Wait up to one second for completion
    result.handler->wait(pdMS_TO_TICKS(1000));
}

void loop() {}
```

- Call `worker.init` once to set library defaults.
- `worker.spawn` accepts any callable (lambda, `std::bind`, function pointer) and an optional `WorkerConfig`.
- The returned `WorkerResult` contains either a joinable `WorkerHandler` or an error code.

---

## Worker Configuration

| Field | Description | Default |
| --- | --- | --- |
| `stackSize` | Stack depth in FreeRTOS words. Use larger values for complex C++ code. | `4096` |
| `priority` | Task priority (`tskIDLE_PRIORITY + 1` is recommended as a baseline). | `1` |
| `coreId` | Core affinity (`tskNO_AFFINITY` let FreeRTOS pick). | `tskNO_AFFINITY` |
| `name` | Helpful for `esp_task_wdt` / logging. Auto-generated (`worker-n`). | Auto |
| `useExternalStack` | Allocate the stack from PSRAM when available. | `false` |

Use `worker.spawnExt` to force `useExternalStack = true` regardless of the passed configuration.

---

## Events, Errors & Diagnostics

```cpp
worker.onEvent([](WorkerEvent event) {
    Serial.printf("[event] %s\n", worker.eventToString(event));
});

worker.onError([](WorkerError error) {
    Serial.printf("[error] %s\n", worker.errorToString(error));
});

WorkerResult job = worker.spawn([]() {
    // ...
});

if (job) {
    WorkerDiag diag = job.handler->getDiag();
    Serial.printf("Task %s running: %d runtime: %lu ms\n",
                  diag.config.name.c_str(), diag.running, static_cast<unsigned long>(diag.runtimeMs));
}
```

Events fire in worker context (`Created → Started → Completed/Destroyed`). The error callback is invoked from the API call that encountered a failure.

---

## Example Sketches

| Example | Highlights |
| --- | --- |
| [`examples/basic_worker`](examples/basic_worker) | Spawning workers, joining, inspecting diagnostics, logging events/errors |
| [`examples/psram_stack`](examples/psram_stack) | Using PSRAM stacks with `spawnExt`, watching completion |

To build all examples locally:

```bash
# PlatformIO
pip install platformio
pio ci examples/basic_worker --board esp32dev --lib=.

# Arduino CLI
arduino-cli core install esp32:esp32
arduino-cli compile --fqbn esp32:esp32:esp32 examples/basic_worker
```

---

## Troubleshooting

- Ensure your project compiles with C++17 (`-std=gnu++17`) and that FreeRTOS is enabled (standard on ESP32).
- `MaxWorkersReached` indicates the configured pool is exhausted—destroy or wait for workers to finish, or raise `maxWorkers` in `ESPWorker::Config`.
- If you need deterministic cleanup consider calling `handler->destroy()` during shutdown.

---

## Contributing

Issues and pull requests are welcome! Please open a discussion if you have questions about the API before implementing major changes. The CI workflow builds every example on multiple ESP32 variants to keep the library portable.

---

## License

ESPWorker is released under the [MIT License](LICENSE.md).

## ESPToolKit

Check out other libraries under ESPToolKit: https://github.com/orgs/ESPToolKit/repositories