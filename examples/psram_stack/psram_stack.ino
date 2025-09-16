#include <Arduino.h>
#include <ESPWorker.h>

#include <cstring>
#include <memory>

namespace {
std::shared_ptr<WorkerHandler> psramWorker;
}

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000) {
        delay(10);
    }

    worker.init({
        .maxWorkers = 3,
        .defaultStackSize = 4096,
        .defaultPriority = 1,
        .defaultCoreId = tskNO_AFFINITY,
        .enableExternalStacks = true,
    });

    worker.onEvent([](WorkerEvent event) {
        Serial.printf("[PSRAM] %s\n", worker.eventToString(event));
    });

    worker.onError([](WorkerError error) {
        Serial.printf("[PSRAM][error] %s\n", worker.errorToString(error));
    });

    WorkerConfig config;
    config.name = "psram-task";
    config.priority = 3;
    config.stackSize = 8192;
    config.useExternalStack = true;

    auto result = worker.spawnExt([]() {
        constexpr size_t kBufferSize = 16 * 1024;
        uint8_t buffer[kBufferSize];
        memset(buffer, 0xA5, sizeof(buffer));
        Serial.printf("[psram-task] filled %u bytes on external stack\n", static_cast<unsigned>(sizeof(buffer)));
        vTaskDelay(pdMS_TO_TICKS(500));
    }, config);

    if (!result) {
        Serial.printf("Failed to spawn psram-task: %s\n", worker.errorToString(result.error));
        return;
    }

    psramWorker = result.handler;
}

void loop() {
    if (psramWorker) {
        psramWorker->wait(pdMS_TO_TICKS(50));
        WorkerDiag diag = psramWorker->getDiag();
        if (!diag.running) {
            Serial.printf("psram-task runtime: %lu ms\n", static_cast<unsigned long>(diag.runtimeMs));
            psramWorker.reset();
        }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}
