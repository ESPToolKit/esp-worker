#include <ESPWorker.h>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "test_support.h"

namespace {

[[noreturn]] void fail(const std::string &message) {
    throw std::runtime_error(message);
}

void expectTrue(bool condition, const std::string &message) {
    if (!condition) {
        fail(message);
    }
}

void expectFalse(bool condition, const std::string &message) {
    if (condition) {
        fail(message);
    }
}

template <typename T>
void expectEqual(const T &actual, const T &expected, const std::string &message) {
    if (!(actual == expected)) {
        fail(message);
    }
}

void testDeinitIsSafeBeforeInit() {
    ESPWorker worker;
    expectFalse(worker.isInitialized(), "worker should start deinitialized");
    expectEqual(worker.activeWorkers(), static_cast<size_t>(0), "worker should start with no active jobs");

    worker.deinit();
    expectFalse(worker.isInitialized(), "deinit before init should be a no-op");
    expectEqual(worker.activeWorkers(), static_cast<size_t>(0), "deinit before init should keep worker count at zero");
}

void testDeinitIsIdempotent() {
    ESPWorker worker;
    ESPWorker::Config cfg{};
    cfg.maxWorkers = 4;
    worker.init(cfg);

    expectTrue(worker.isInitialized(), "worker should be initialized");

    worker.deinit();
    expectFalse(worker.isInitialized(), "worker should deinitialize cleanly");
    expectEqual(worker.activeWorkers(), static_cast<size_t>(0), "deinit should clear worker state");

    worker.deinit();
    expectFalse(worker.isInitialized(), "second deinit should remain safe");
    expectEqual(worker.activeWorkers(), static_cast<size_t>(0), "second deinit should not reintroduce workers");
}

void testReinitLifecycleAfterDeinit() {
    test_support::resetRuntime();

    ESPWorker worker;
    ESPWorker::Config cfg{};
    cfg.maxWorkers = 2;
    worker.init(cfg);
    expectTrue(worker.isInitialized(), "first init should succeed");

    WorkerResult first = worker.spawn([]() {});
    expectTrue(static_cast<bool>(first), "spawn after first init should succeed");
    expectEqual(worker.activeWorkers(), static_cast<size_t>(1), "first spawn should reserve one worker slot");

    worker.deinit();
    expectFalse(worker.isInitialized(), "worker should be deinitialized after first lifecycle");
    expectEqual(worker.activeWorkers(), static_cast<size_t>(0), "deinit should clear active workers");

    worker.init(cfg);
    expectTrue(worker.isInitialized(), "second init should succeed");

    WorkerResult second = worker.spawn([]() {});
    expectTrue(static_cast<bool>(second), "spawn after reinit should succeed");
    expectEqual(worker.activeWorkers(), static_cast<size_t>(1), "second spawn should reserve one worker slot");

    worker.deinit();
}

void testDeinitReleasesActiveTaskHandles() {
    test_support::resetRuntime();

    ESPWorker worker;
    worker.init(ESPWorker::Config{});
    WorkerResult first = worker.spawn([]() {});
    WorkerResult second = worker.spawn([]() {});

    expectTrue(static_cast<bool>(first), "first spawn should succeed");
    expectTrue(static_cast<bool>(second), "second spawn should succeed");
    expectEqual(worker.activeWorkers(), static_cast<size_t>(2), "two workers should be active before teardown");
    expectEqual(test_support::createdTaskCount(), static_cast<size_t>(2), "stubs should observe two created tasks");

    worker.deinit();

    expectFalse(worker.isInitialized(), "worker should be deinitialized");
    expectEqual(worker.activeWorkers(), static_cast<size_t>(0), "deinit should clear active workers");
    expectEqual(test_support::deletedTaskCount(), static_cast<size_t>(2), "deinit should release all active task handles");
}

void testDestructorDelegatesToDeinit() {
    test_support::resetRuntime();

    {
        ESPWorker worker;
        worker.init(ESPWorker::Config{});
        WorkerResult result = worker.spawn([]() {});
        expectTrue(static_cast<bool>(result), "spawn should succeed for destructor test");
        expectEqual(worker.activeWorkers(), static_cast<size_t>(1), "one worker should be active before scope exit");
    }

    expectEqual(test_support::deletedTaskCount(), static_cast<size_t>(1), "destructor should deinit and release active worker");
}

}  // namespace

int main() {
    try {
        testDeinitIsSafeBeforeInit();
        testDeinitIsIdempotent();
        testReinitLifecycleAfterDeinit();
        testDeinitReleasesActiveTaskHandles();
        testDestructorDelegatesToDeinit();
    } catch (const std::exception &ex) {
        std::cerr << "FAIL: " << ex.what() << '\n';
        return 1;
    }

    std::cout << "All esp-worker lifecycle tests passed\n";
    return 0;
}
