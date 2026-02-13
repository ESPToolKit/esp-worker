#include "worker.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

extern "C" {
#include "esp_heap_caps.h"
}

#if __has_include("freertos/idf_additions.h")
extern "C" {
#include "freertos/idf_additions.h"
}
#define ESPWORKER_HAS_IDF_TASK_CAPS 1
#else
#define ESPWORKER_HAS_IDF_TASK_CAPS 0
#endif

#if ESPWORKER_HAS_IDF_TASK_CAPS && defined(configSUPPORT_STATIC_ALLOCATION) && (configSUPPORT_STATIC_ALLOCATION == 1) && defined(MALLOC_CAP_SPIRAM)
#define ESPWORKER_CAN_USE_EXTERNAL_STACKS 1
#else
#define ESPWORKER_CAN_USE_EXTERNAL_STACKS 0
#endif

namespace {
constexpr size_t kMinStackSizeBytes = 1024;
constexpr UBaseType_t kInternalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#if defined(MALLOC_CAP_SPIRAM)
constexpr UBaseType_t kExternalStackCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
constexpr UBaseType_t kExternalStackCaps = MALLOC_CAP_8BIT;
#endif

template <typename T, typename... Args>
std::shared_ptr<T> makeInternalShared(Args &&...args) {
    void *raw = heap_caps_malloc(sizeof(T), kInternalCaps);
    if (!raw) {
        return {};
    }

    T *object = new (raw) T(std::forward<Args>(args)...);
    return std::shared_ptr<T>(object, [](T *ptr) {
        if (!ptr) {
            return;
        }
        ptr->~T();
        heap_caps_free(ptr);
    });
}

bool hasExternalStackSupport() {
#if ESPWORKER_CAN_USE_EXTERNAL_STACKS
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
#else
    return false;
#endif
}

bool isValidStackConfig(size_t stackBytes) {
    if (stackBytes < kMinStackSizeBytes) {
        return false;
    }
    return (stackBytes % sizeof(StackType_t)) == 0;
}

void deleteTaskHandle(TaskHandle_t taskHandle, bool withCaps) {
    if (!taskHandle) {
        return;
    }
#if ESPWORKER_CAN_USE_EXTERNAL_STACKS
    if (withCaps) {
        vTaskDeleteWithCaps(taskHandle);
        return;
    }
#endif
    vTaskDelete(taskHandle);
}

void deleteCurrentTask(bool withCaps) {
#if ESPWORKER_CAN_USE_EXTERNAL_STACKS
    if (withCaps) {
        vTaskDeleteWithCaps(xTaskGetCurrentTaskHandle());
        return;
    }
#endif
    vTaskDelete(nullptr);
}
}  // namespace

static_assert(kESPWorkerDefaultStackSizeBytes >= kMinStackSizeBytes, "Default stack size must be at least 1024 bytes.");
static_assert((kESPWorkerDefaultStackSizeBytes % sizeof(StackType_t)) == 0, "Default stack size must be aligned to StackType_t.");

struct WorkerHandler::Impl {
    ESPWorker *owner{nullptr};
    ESPWorker::TaskCallback callback{};
    WorkerConfig config{};

    TaskHandle_t taskHandle{nullptr};
    TickType_t startTick{0};
    TickType_t endTick{0};

    SemaphoreHandle_t completion{nullptr};
    StaticSemaphore_t completionBuffer{};

    bool createdWithCaps{false};

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
}

ESPWorker::~ESPWorker() {
    deinit();
}

void ESPWorker::deinit() {
    std::vector<std::shared_ptr<WorkerHandler::Impl>> controls;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        controls = _activeControls;
    }

    for (auto &control : controls) {
        if (!control) {
            continue;
        }

        if (control->taskHandle && xTaskGetCurrentTaskHandle() != control->taskHandle) {
            deleteTaskHandle(control->taskHandle, control->createdWithCaps);
        }
        finalizeWorker(control, true);
        control->owner = nullptr;
    }

    {
        std::lock_guard<std::mutex> guard(_mutex);
        _activeControls.clear();
    }

    {
        std::lock_guard<std::mutex> guard(_callbackMutex);
        _eventCallback = nullptr;
        _errorCallback = nullptr;
    }

    _initialized = false;
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
    if (effective.stackSizeBytes == 0) {
        effective.stackSizeBytes = _config.stackSizeBytes;
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

    if (!isValidStackConfig(config.stackSizeBytes)) {
        notifyError(WorkerError::InvalidConfig);
        return {WorkerError::InvalidConfig, {}, "stackSizeBytes must be >= 1024 and aligned to StackType_t"};
    }

    if (config.useExternalStack) {
        if (!_config.enableExternalStacks) {
            notifyError(WorkerError::ExternalStackUnsupported);
            return {WorkerError::ExternalStackUnsupported, {}, "External stacks are disabled in ESPWorker::Config"};
        }
        if (!hasExternalStackSupport()) {
            notifyError(WorkerError::ExternalStackUnsupported);
            return {WorkerError::ExternalStackUnsupported, {}, "External stack mode is not supported on this target"};
        }
    }

    auto control = makeInternalShared<WorkerHandler::Impl>();
    if (!control) {
        notifyError(WorkerError::NoMemory);
        return {WorkerError::NoMemory, {}, "Failed to allocate worker control in internal RAM"};
    }
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

    const size_t stackBytes = control->config.stackSizeBytes;
    BaseType_t createResult = pdFAIL;

    if (control->config.useExternalStack) {
#if ESPWORKER_CAN_USE_EXTERNAL_STACKS
        createResult = xTaskCreatePinnedToCoreWithCaps(taskTrampoline,
                                                       control->config.name.c_str(),
                                                       static_cast<configSTACK_DEPTH_TYPE>(stackBytes),
                                                       control.get(),
                                                       control->config.priority,
                                                       &control->taskHandle,
                                                       control->config.coreId,
                                                       kExternalStackCaps);
        control->createdWithCaps = (createResult == pdPASS);
#else
        createResult = pdFAIL;
#endif
    } else {
        createResult = xTaskCreatePinnedToCore(taskTrampoline, control->config.name.c_str(), static_cast<uint32_t>(stackBytes), control.get(), control->config.priority, &control->taskHandle, control->config.coreId);
        control->createdWithCaps = false;
    }

    if (createResult != pdPASS) {
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
    const bool createdWithCaps = controlPtr->createdWithCaps;

    std::shared_ptr<WorkerHandler::Impl> control = controlPtr->self.lock();
    if (!control || !control->owner) {
        vTaskDelete(nullptr);
        return;
    }

    control->owner->notifyEvent(WorkerEvent::Started);
    control->owner->runTask(std::move(control));
    deleteCurrentTask(createdWithCaps);
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

    if (control->taskHandle) {
        control->taskHandle = nullptr;
    }

    if (control->completion) {
        xSemaphoreGive(control->completion);
    }

    {
        std::lock_guard<std::mutex> guard(_mutex);
        _activeControls.erase(std::remove_if(_activeControls.begin(), _activeControls.end(), [&](const auto &ptr) { return ptr.get() == control.get(); }), _activeControls.end());
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

    deleteTaskHandle(control->taskHandle, control->createdWithCaps);
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
        case WorkerError::ExternalStackUnsupported:
            return "ExternalStackUnsupported";
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
