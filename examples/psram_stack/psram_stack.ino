#include <Arduino.h>
#include <ESPWorker.h>

#include <cstring>
#include <memory>

ESPWorker worker;

ESPWorker::Config workerConfig = {
	.maxWorkers = 16,
	.stackSizeBytes = 2048,
	.priority = 1,
	.coreId = tskNO_AFFINITY,
	.enableExternalStacks = true,
};

void printPSRAM(const char* tag){
    uint32_t freePSRAM = ESP.getFreePsram();
    Serial.printf("[%s] Free PSRAM: %u bytes\n", tag, freePSRAM);
}

void setup() {
    Serial.begin(115200);
    while (!Serial) {}

    worker.init(workerConfig);

    printPSRAM("Initial");

    // Spawn a default job with psram stack
    auto testJob = worker.spawnExt([](){
        Serial.println("[Worker] This task stack uses PSRAM!");
        printPSRAM("Inside job");
    });

    testJob.handler->wait(); // Wait for the job to finish, indefinietly
    printPSRAM("After job completed");
}

void loop() {}
