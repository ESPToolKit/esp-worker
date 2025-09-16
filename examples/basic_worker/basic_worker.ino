#include <Arduino.h>
#include <ESPWorker.h>

#include <memory>

namespace {
std::shared_ptr<WorkerHandler> heavyWorker;
}

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000) {
        delay(10);
    }

    ESPWorker::Config config;
    config.maxWorkers = 4;
    config.defaultStackSize = 4096;
    config.defaultPriority = 1;
    config.defaultCoreId = tskNO_AFFINITY;
    config.enableExternalStacks = true;
    worker.init(config);

    worker.onEvent([](WorkerEvent event) {
        Serial.printf("[ESPWorker][Event] %s\n", worker.eventToString(event));
    });

    worker.onError([](WorkerError error) {
        Serial.printf("[ESPWorker][Error] %s\n", worker.errorToString(error));
    });

    WorkerConfig heavyConfig;
    heavyConfig.name = "heavy-loop";
    heavyConfig.priority = 4;
    heavyConfig.stackSize = 4096;

    auto heavy = worker.spawn([]() {
        for (int i = 0; i < 5; ++i) {
            Serial.printf("[heavy-loop] iteration %d\n", i);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }, heavyConfig);

    if (!heavy) {
        Serial.printf("Failed to start heavy-loop: %s\n", worker.errorToString(heavy.error));
    } else {
        heavyWorker = heavy.handler;
    }

    auto onetime = worker.spawn([]() {
        Serial.println("[once] running in worker context");
        vTaskDelay(pdMS_TO_TICKS(100));
    });

    if (!onetime) {
        Serial.printf("Failed to start onetime: %s\n", worker.errorToString(onetime.error));
    } else {
        onetime.handler->wait(pdMS_TO_TICKS(1000));
    }
}

void loop() {
    static bool reported = false;
    if (heavyWorker && !reported) {
        WorkerDiag diag = heavyWorker->getDiag();
        if (!diag.running) {
            Serial.printf("heavy-loop finished after %lu ms\n", static_cast<unsigned long>(diag.runtimeMs));
            reported = true;
            heavyWorker.reset();
        }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
}
