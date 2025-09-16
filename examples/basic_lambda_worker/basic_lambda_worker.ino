#include <Arduino.h>
#include <ESPWorker.h>

ESPWorker::Config workerConfig = {
	.maxWorkers = 16,
	.defaultStackSize = 2048,
	.defaultPriority = 1,
	.defaultCoreId = tskNO_AFFINITY,
	.enableExternalStacks = true,
};

void setup() {
    Serial.begin(115200);
    while (!Serial) {}

    // Spawn a default job
    auto testJob = worker.spawn([](){
		Serial.println("[Worker] task is triggered!");
		vTaskDelay(pdMS_TO_TICKS(1000));
	});
	testJob.handler->wait(); // Wait for the job to finish, indefinietly
	Serial.println("[Worker] task is completed!");
}

void loop() {}
