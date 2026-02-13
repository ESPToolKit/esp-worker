#include <Arduino.h>
#include <ESPWorker.h>

ESPWorker worker;

ESPWorker::Config workerConfig = {
	.maxWorkers = 16,
	.stackSizeBytes = 2048,
	.priority = 1,
	.coreId = tskNO_AFFINITY,
	.enableExternalStacks = true,
};

void setup() {
    Serial.begin(115200);
    while (!Serial) {}

    worker.init(workerConfig);

    // Spawn a default job
    auto testJob = worker.spawn([](){
		Serial.println("[Worker] task is triggered!");
		vTaskDelay(pdMS_TO_TICKS(1000));
	});
	testJob.handler->wait(); // Wait for the job to finish, indefinietly
	Serial.println("[Worker] task is completed!");
}

void loop() {}
