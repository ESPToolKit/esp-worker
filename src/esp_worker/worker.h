#pragma once

#include <Arduino.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
}

class WorkerHandler;
class ESPWorker;

struct WorkerConfig {
    size_t stackSize = 4096 * sizeof(StackType_t);  // Task stack size in bytes
    UBaseType_t priority = 1;               // FreeRTOS task priority
    BaseType_t coreId = tskNO_AFFINITY;     // preferred core, or tskNO_AFFINITY for any
    std::string name{};                     // optional task name
    bool useExternalStack = false;          // request PSRAM backed stack for the task
};

struct JobDiag {
    WorkerConfig config{};
    uint32_t runtimeMs = 0;
    bool running = false;
    bool destroyed = false;
    TaskHandle_t taskHandle = nullptr;
};

struct WorkerDiag {
    size_t totalJobs = 0;
    size_t runningJobs = 0;
    size_t waitingJobs = 0;
    size_t psramStackJobs = 0;
    uint32_t averageRuntimeMs = 0;
    uint32_t maxRuntimeMs = 0;
};

enum class WorkerError {
    None = 0,
    NotInitialized,
    InvalidConfig,
    MaxWorkersReached,
    TaskCreateFailed,
    NoMemory,
};

enum class WorkerEvent {
    Created = 0,
    Started,
    Completed,
    Destroyed,
};

class WorkerHandler {
   public:
    WorkerHandler() = default;

    bool valid() const;
    JobDiag getDiag() const;
    bool wait(TickType_t ticks = portMAX_DELAY);
    bool destroy();

   private:
    struct Impl;
    friend class ESPWorker;
    explicit WorkerHandler(std::shared_ptr<Impl> control);

    std::shared_ptr<Impl> _control{};
};

struct WorkerResult {
    WorkerError error{WorkerError::None};
    std::shared_ptr<WorkerHandler> handler{};
    const char *message{nullptr};

    explicit operator bool() const { return error == WorkerError::None; }
};

class ESPWorker {
    friend class WorkerHandler;

   public:
    using TaskCallback = std::function<void()>;
    using EventCallback = std::function<void(WorkerEvent)>;
    using ErrorCallback = std::function<void(WorkerError)>;

    struct Config {
        size_t maxWorkers = 8;
        size_t stackSize = 4096 * sizeof(StackType_t);
        UBaseType_t priority = 1;
        BaseType_t coreId = tskNO_AFFINITY;
        bool enableExternalStacks = true;
    };

    ESPWorker() = default;

    void init(const Config &config);

    WorkerResult spawn(TaskCallback callback, const WorkerConfig &config = WorkerConfig{});
    WorkerResult spawnExt(TaskCallback callback, const WorkerConfig &config = WorkerConfig{});

    size_t activeWorkers() const;
    void cleanupFinished();

    WorkerDiag getDiag() const;

    void onEvent(EventCallback callback);
    void onError(ErrorCallback callback);

    const char *eventToString(WorkerEvent event) const;
    const char *errorToString(WorkerError error) const;

   private:
    WorkerResult spawnInternal(TaskCallback &&callback, WorkerConfig config);
    static void taskTrampoline(void *arg);

    void runTask(std::shared_ptr<WorkerHandler::Impl> control);
    void finalizeWorker(const std::shared_ptr<WorkerHandler::Impl> &control, bool destroyed);
    bool destroyWorker(const std::shared_ptr<WorkerHandler::Impl> &control);
    std::string makeName();
    void notifyEvent(WorkerEvent event);
    void notifyError(WorkerError error);

    Config _config{};
    bool _initialized = false;

    mutable std::mutex _mutex;
    std::vector<std::shared_ptr<WorkerHandler::Impl>> _activeControls;

    mutable std::mutex _callbackMutex;
    EventCallback _eventCallback{};
    ErrorCallback _errorCallback{};
};
