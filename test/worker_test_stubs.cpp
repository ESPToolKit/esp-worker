#include "Arduino.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "test_support.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <new>
#include <unordered_set>

namespace {

struct FakeSemaphore {
	std::atomic<bool> available{false};
};

struct FakeTask {
	TaskFunction_t entry{nullptr};
	void *arg{nullptr};
};

std::atomic<TickType_t> g_tickCount{0};
std::atomic<size_t> g_createdTasks{0};
std::atomic<size_t> g_deletedTasks{0};

std::mutex g_taskMutex;
std::unordered_set<TaskHandle_t> g_liveTasks;

TaskHandle_t g_currentTaskHandle = nullptr;

} // namespace

extern "C" unsigned long millis(void) {
	return static_cast<unsigned long>(g_tickCount.load(std::memory_order_relaxed));
}

extern "C" SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t * /*buffer*/) {
	auto *sem = new (std::nothrow) FakeSemaphore{};
	return reinterpret_cast<SemaphoreHandle_t>(sem);
}

extern "C" BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t /*ticks*/) {
	if (!handle) {
		return pdFALSE;
	}
	auto *sem = reinterpret_cast<FakeSemaphore *>(handle);
	bool expected = true;
	if (sem->available.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
		return pdTRUE;
	}
	return pdFALSE;
}

extern "C" BaseType_t xSemaphoreGive(SemaphoreHandle_t handle) {
	if (!handle) {
		return pdFALSE;
	}
	auto *sem = reinterpret_cast<FakeSemaphore *>(handle);
	sem->available.store(true, std::memory_order_release);
	return pdTRUE;
}

extern "C" void vSemaphoreDelete(SemaphoreHandle_t handle) {
	auto *sem = reinterpret_cast<FakeSemaphore *>(handle);
	delete sem;
}

extern "C" BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t task,
    const char * /*name*/,
    uint32_t /*stackDepth*/,
    void *parameters,
    UBaseType_t /*priority*/,
    TaskHandle_t *createdTask,
    BaseType_t /*coreId*/
) {
	auto *fakeTask = new (std::nothrow) FakeTask{task, parameters};
	if (!fakeTask) {
		return pdFAIL;
	}

	TaskHandle_t handle = reinterpret_cast<TaskHandle_t>(fakeTask);
	if (createdTask) {
		*createdTask = handle;
	}

	{
		std::lock_guard<std::mutex> guard(g_taskMutex);
		g_liveTasks.insert(handle);
	}

	g_createdTasks.fetch_add(1, std::memory_order_relaxed);
	return pdPASS;
}

extern "C" void vTaskDelete(TaskHandle_t task) {
	TaskHandle_t target = task ? task : g_currentTaskHandle;
	if (!target) {
		return;
	}

	bool removed = false;
	{
		std::lock_guard<std::mutex> guard(g_taskMutex);
		removed = g_liveTasks.erase(target) > 0;
	}

	if (removed) {
		g_deletedTasks.fetch_add(1, std::memory_order_relaxed);
		auto *fakeTask = reinterpret_cast<FakeTask *>(target);
		delete fakeTask;
	}
}

extern "C" void vTaskDelay(TickType_t ticks) {
	g_tickCount.fetch_add(ticks, std::memory_order_relaxed);
}

extern "C" TickType_t xTaskGetTickCount(void) {
	return g_tickCount.load(std::memory_order_relaxed);
}

extern "C" TaskHandle_t xTaskGetCurrentTaskHandle(void) {
	return g_currentTaskHandle;
}

extern "C" void *heap_caps_malloc(size_t size, unsigned int /*caps*/) {
	return std::malloc(size);
}

extern "C" void heap_caps_free(void *ptr) {
	std::free(ptr);
}

extern "C" size_t heap_caps_get_total_size(unsigned int /*caps*/) {
	return 0;
}

namespace test_support {

void resetRuntime() {
	g_tickCount.store(0, std::memory_order_relaxed);
	g_createdTasks.store(0, std::memory_order_relaxed);
	g_deletedTasks.store(0, std::memory_order_relaxed);

	std::lock_guard<std::mutex> guard(g_taskMutex);
	for (TaskHandle_t handle : g_liveTasks) {
		auto *fakeTask = reinterpret_cast<FakeTask *>(handle);
		delete fakeTask;
	}
	g_liveTasks.clear();
}

size_t createdTaskCount() {
	return g_createdTasks.load(std::memory_order_relaxed);
}

size_t deletedTaskCount() {
	return g_deletedTasks.load(std::memory_order_relaxed);
}

} // namespace test_support
