#include "worker.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

extern "C" {
#include "esp_heap_caps.h"
#include "esp_freertos_hooks.h"
}

namespace {
struct DeferredFree {
    StackType_t *stack{nullptr};
    StaticTask_t *tcb{nullptr};
};

std::mutex &deferredMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<DeferredFree> &deferredList() {
    static std::vector<DeferredFree> list;
    return list;
}

void scheduleDeferredFree(StackType_t *stack, StaticTask_t *tcb) {
    if (!stack && !tcb) {
        return;
    }
    std::lock_guard<std::mutex> guard(deferredMutex());
    deferredList().push_back({stack, tcb});
}

bool idleDeferredFreeHook() {
    std::vector<DeferredFree> pending;
    {
        std::lock_guard<std::mutex> guard(deferredMutex());
        if (deferredList().empty()) {
            return true;
        }
        pending.swap(deferredList());
    }

    for (auto &entry : pending) {
        if (entry.stack) {
            heap_caps_free(entry.stack);
        }
        if (entry.tcb) {
            heap_caps_free(entry.tcb);
        }
    }

    return true;
}

void ensureIdleHookRegistered() {
    static std::once_flag once;
    std::call_once(once, []() {
        esp_register_freertos_idle_hook_for_cpu(idleDeferredFreeHook, 0);
        esp_register_freertos_idle_hook_for_cpu(idleDeferredFreeHook, 1);
    });
}
}  // namespace

struct WorkerHandler::Impl {
    ESPWorker *owner{nullptr};
    ESPWorker::TaskCallback callback{};
    WorkerConfig config{};

    TaskHandle_t taskHandle{nullptr};
    TickType_t startTick{0};
    TickType_t endTick{0};

    SemaphoreHandle_t completion{nullptr};
    StaticSemaphore_t completionBuffer{};

    StackType_t *externalStack{nullptr};
    StaticTask_t *externalTaskBuffer{nullptr};

    std::atomic<bool> running{false};
    std::atomic<bool> destroyed{false};
    std::atomic<bool> finalized{false};

    std::weak_ptr<Impl> self;

    ~Impl();
};

WorkerHandler::WorkerHandler(std::shared_ptr<Impl> control) : _control(std::move(control)) {}

WorkerHandler::Impl::~Impl() {
    if (completion) {
        vSemaphoreDelete(completion);
        completion = nullptr;
    }
    if (externalStack || externalTaskBuffer) {
        scheduleDeferredFree(externalStack, externalTaskBuffer);
        externalStack = nullptr;
        externalTaskBuffer = nullptr;
    }
}

bool WorkerHandler::valid() const { return static_cast<bool>(_control); }

JobDiag WorkerHandler::getDiag() const {
    JobDiag diag{};
    if (!_control) {
        return diag;
    }

    diag.config = _control->config;
    diag.taskHandle = _control->taskHandle;
    diag.running = _control->running.load(std::memory_order_acquire);
    diag.destroyed = _control->destroyed.load(std::memory_order_acquire);

    TickType_t endTicks = diag.running ? xTaskGetTickCount() : _control->endTick;
    if (endTicks >= _control->startTick) {
        TickType_t elapsedTicks = endTicks - _control->startTick;
        diag.runtimeMs = static_cast<uint32_t>(elapsedTicks * portTICK_PERIOD_MS);
    }

    return diag;
}

bool WorkerHandler::wait(TickType_t ticks) {
    if (!_control) {
        return false;
    }
    std::shared_ptr<Impl> control = _control;
    if (!control->completion) {
        return false;
    }

    if (!control->running.load(std::memory_order_acquire)) {
        return true;
    }

    if (xSemaphoreTake(control->completion, ticks) == pdTRUE) {
        return true;
    }

    return !control->running.load(std::memory_order_acquire);
}

bool WorkerHandler::destroy() {
    if (!_control) {
        return false;
    }
    std::shared_ptr<Impl> control = _control;
    if (!control->owner) {
        return false;
    }
    return control->owner->destroyWorker(control);
}

void ESPWorker::init(const Config &config) {
    std::lock_guard<std::mutex> guard(_mutex);
    _config = config;
    _initialized = true;
}

WorkerResult ESPWorker::spawn(TaskCallback callback, const WorkerConfig &config) {
    if (!_initialized) {
        init(Config{});
    }
    WorkerConfig effective = config;
    effective.useExternalStack = effective.useExternalStack && _config.enableExternalStacks;
    if (effective.stackSize == 0) {
        effective.stackSize = _config.stackSize;
    }
    if (effective.priority == 0) {
        effective.priority = _config.priority;
    }
    if (effective.coreId == tskNO_AFFINITY) {
        effective.coreId = _config.coreId;
    }
    if (effective.name.empty()) {
        effective.name = makeName();
    }

    return spawnInternal(std::move(callback), std::move(effective));
}

WorkerResult ESPWorker::spawnExt(TaskCallback callback, const WorkerConfig &config) {
    WorkerConfig extConfig = config;
    extConfig.useExternalStack = true;
    return spawn(std::move(callback), extConfig);
}

WorkerResult ESPWorker::spawnInternal(TaskCallback &&callback, WorkerConfig config) {
    if (!callback) {
        notifyError(WorkerError::InvalidConfig);
        return {WorkerError::InvalidConfig, {}, "Callback must be callable"};
    }

    auto control = std::make_shared<WorkerHandler::Impl>();
    control->owner = this;
    control->callback = std::move(callback);
    control->config = std::move(config);

    control->completion = xSemaphoreCreateBinaryStatic(&control->completionBuffer);
    if (!control->completion) {
        notifyError(WorkerError::NoMemory);
        return {WorkerError::NoMemory, {}, "Failed to create completion semaphore"};
    }

    control->self = control;

    bool limitReached = false;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        if (_activeControls.size() >= _config.maxWorkers) {
            limitReached = true;
        } else {
            _activeControls.push_back(control);
        }
    }

    if (limitReached) {
        notifyError(WorkerError::MaxWorkersReached);
        return {WorkerError::MaxWorkersReached, {}, "Maximum workers reached"};
    }

    BaseType_t createResult = pdFAIL;
    const size_t stackBytes = control->config.stackSize;

    if (control->config.useExternalStack) {
        ensureIdleHookRegistered();
        control->externalStack = static_cast<StackType_t *>(heap_caps_malloc(stackBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        control->externalTaskBuffer = static_cast<StaticTask_t *>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!control->externalStack || !control->externalTaskBuffer) {
            heap_caps_free(control->externalStack);
            heap_caps_free(control->externalTaskBuffer);
            {
                std::lock_guard<std::mutex> guard(_mutex);
                _activeControls.erase(std::remove_if(_activeControls.begin(), _activeControls.end(), [&](const auto &ptr) { return ptr.get() == control.get(); }), _activeControls.end());
            }
            notifyError(WorkerError::NoMemory);
            return {WorkerError::NoMemory, {}, "Failed to allocate external stack"};
        }

        control->taskHandle = xTaskCreateStaticPinnedToCore(taskTrampoline, control->config.name.c_str(), static_cast<uint32_t>(stackBytes), control.get(), control->config.priority, control->externalStack, control->externalTaskBuffer, control->config.coreId);
        createResult = control->taskHandle ? pdPASS : pdFAIL;
    } else {
        createResult = xTaskCreatePinnedToCore(taskTrampoline, control->config.name.c_str(), static_cast<uint32_t>(stackBytes), control.get(), control->config.priority, &control->taskHandle, control->config.coreId);
    }

    if (createResult != pdPASS) {
        heap_caps_free(control->externalStack);
        heap_caps_free(control->externalTaskBuffer);

        {
            std::lock_guard<std::mutex> guard(_mutex);
            _activeControls.erase(std::remove_if(_activeControls.begin(), _activeControls.end(), [&](const auto &ptr) { return ptr.get() == control.get(); }), _activeControls.end());
        }

        notifyError(WorkerError::TaskCreateFailed);
        return {WorkerError::TaskCreateFailed, {}, "Failed to create worker task"};
    }

    control->running.store(true, std::memory_order_release);
    control->startTick = xTaskGetTickCount();

    auto handler = std::shared_ptr<WorkerHandler>(new WorkerHandler(control));
    notifyEvent(WorkerEvent::Created);
    return {WorkerError::None, handler, nullptr};
}

void ESPWorker::taskTrampoline(void *arg) {
    auto *controlPtr = static_cast<WorkerHandler::Impl *>(arg);
    if (!controlPtr) {
        vTaskDelete(nullptr);
        return;
    }

    std::shared_ptr<WorkerHandler::Impl> control = controlPtr->self.lock();
    if (!control || !control->owner) {
        vTaskDelete(nullptr);
        return;
    }

    control->owner->notifyEvent(WorkerEvent::Started);
    control->owner->runTask(std::move(control));
    vTaskDelete(nullptr);
}

void ESPWorker::runTask(std::shared_ptr<WorkerHandler::Impl> control) {
    auto callback = std::move(control->callback);
    if (callback) {
        callback();
    }
    finalizeWorker(control, false);
}

void ESPWorker::finalizeWorker(const std::shared_ptr<WorkerHandler::Impl> &control, bool destroyed) {
    if (!control) {
        return;
    }

    bool expected = false;
    if (!control->finalized.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    control->destroyed.store(destroyed, std::memory_order_release);
    control->running.store(false, std::memory_order_release);
    control->endTick = xTaskGetTickCount();

    TaskHandle_t taskHandle = control->taskHandle;
    bool freeExternalNow = true;
    if (taskHandle && xTaskGetCurrentTaskHandle() == taskHandle) {
        // The worker is finalizing itself, its stack is still in use until vTaskDelete runs.
        freeExternalNow = false;
    }
    if (taskHandle) {
        control->taskHandle = nullptr;
    }

    if (control->completion) {
        xSemaphoreGive(control->completion);
    }

    StackType_t *externalStack = nullptr;
    StaticTask_t *externalTaskBuffer = nullptr;

    {
        std::lock_guard<std::mutex> guard(_mutex);
        _activeControls.erase(std::remove_if(_activeControls.begin(), _activeControls.end(), [&](const auto &ptr) { return ptr.get() == control.get(); }), _activeControls.end());
        if (freeExternalNow) {
            externalStack = control->externalStack;
            control->externalStack = nullptr;
            externalTaskBuffer = control->externalTaskBuffer;
            control->externalTaskBuffer = nullptr;
        }
    }

    if (freeExternalNow && (externalStack || externalTaskBuffer)) {
        scheduleDeferredFree(externalStack, externalTaskBuffer);
    }

    notifyEvent(destroyed ? WorkerEvent::Destroyed : WorkerEvent::Completed);
}

bool ESPWorker::destroyWorker(const std::shared_ptr<WorkerHandler::Impl> &control) {
    if (!control) {
        return false;
    }

    if (!control->running.load(std::memory_order_acquire)) {
        return true;
    }

    if (control->taskHandle == nullptr) {
        finalizeWorker(control, true);
        return true;
    }

    if (xTaskGetCurrentTaskHandle() == control->taskHandle) {
        notifyError(WorkerError::InvalidConfig);
        return false;
    }

    vTaskDelete(control->taskHandle);
    finalizeWorker(control, true);
    return true;
}

size_t ESPWorker::activeWorkers() const {
    std::lock_guard<std::mutex> guard(_mutex);
    return _activeControls.size();
}

void ESPWorker::cleanupFinished() {
    std::lock_guard<std::mutex> guard(_mutex);
    _activeControls.erase(std::remove_if(_activeControls.begin(), _activeControls.end(), [](const auto &ptr) {
                            return !ptr || !ptr->running.load(std::memory_order_acquire);
                        }),
                          _activeControls.end());
}

WorkerDiag ESPWorker::getDiag() const {
    WorkerDiag diag{};

    std::vector<std::shared_ptr<WorkerHandler::Impl>> activeControls;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        activeControls = _activeControls;
    }

    diag.totalJobs = activeControls.size();
    if (activeControls.empty()) {
        return diag;
    }

    TickType_t now = xTaskGetTickCount();
    uint64_t runtimeSum = 0;
    bool haveRuntime = false;

    for (const auto &control : activeControls) {
        if (!control) {
            continue;
        }

        bool running = control->running.load(std::memory_order_acquire);
        if (running) {
            diag.runningJobs++;
        }

        if (control->config.useExternalStack) {
            diag.psramStackJobs++;
        }

        TickType_t endTicks = running ? now : control->endTick;
        if (endTicks >= control->startTick) {
            TickType_t elapsedTicks = endTicks - control->startTick;
            uint32_t runtimeMs = static_cast<uint32_t>(elapsedTicks * portTICK_PERIOD_MS);
            runtimeSum += runtimeMs;
            diag.maxRuntimeMs = std::max(diag.maxRuntimeMs, runtimeMs);
            haveRuntime = true;
        }
    }

    if (diag.totalJobs > diag.runningJobs) {
        diag.waitingJobs = diag.totalJobs - diag.runningJobs;
    }

    if (diag.totalJobs > 0 && haveRuntime) {
        diag.averageRuntimeMs = static_cast<uint32_t>(runtimeSum / diag.totalJobs);
    }

    return diag;
}

void ESPWorker::onEvent(EventCallback callback) {
    std::lock_guard<std::mutex> guard(_callbackMutex);
    _eventCallback = std::move(callback);
}

void ESPWorker::onError(ErrorCallback callback) {
    std::lock_guard<std::mutex> guard(_callbackMutex);
    _errorCallback = std::move(callback);
}

const char *ESPWorker::eventToString(WorkerEvent event) const {
    switch (event) {
        case WorkerEvent::Created:
            return "Created";
        case WorkerEvent::Started:
            return "Started";
        case WorkerEvent::Completed:
            return "Completed";
        case WorkerEvent::Destroyed:
            return "Destroyed";
        default:
            return "Unknown";
    }
}

const char *ESPWorker::errorToString(WorkerError error) const {
    switch (error) {
        case WorkerError::None:
            return "None";
        case WorkerError::NotInitialized:
            return "NotInitialized";
        case WorkerError::InvalidConfig:
            return "InvalidConfig";
        case WorkerError::MaxWorkersReached:
            return "MaxWorkersReached";
        case WorkerError::TaskCreateFailed:
            return "TaskCreateFailed";
        case WorkerError::NoMemory:
            return "NoMemory";
        default:
            return "Unknown";
    }
}

std::string ESPWorker::makeName() {
    static std::atomic<uint32_t> counter{0};
    uint32_t id = counter.fetch_add(1, std::memory_order_relaxed);
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "worker-%u", id);
    return std::string(buffer);
}

void ESPWorker::notifyEvent(WorkerEvent event) {
    EventCallback callback;
    {
        std::lock_guard<std::mutex> guard(_callbackMutex);
        callback = _eventCallback;
    }
    if (callback) {
        callback(event);
    }
}

void ESPWorker::notifyError(WorkerError error) {
    if (error == WorkerError::None) {
        return;
    }
    ErrorCallback callback;
    {
        std::lock_guard<std::mutex> guard(_callbackMutex);
        callback = _errorCallback;
    }
    if (callback) {
        callback(error);
    }
}
