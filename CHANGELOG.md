# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Added
- Expose pooled worker metrics via new `ESPWorker::getDiag()` and an expanded `WorkerDiag` struct for aggregated statistics.

### Changed
- Breaking: `WorkerHandler::getDiag()` now returns a `JobDiag`; rename existing `WorkerDiag` usages to the new type.
- Breaking: The inline global `worker` instance has been removed—declare your own `ESPWorker` (or subclass) before spawning tasks.

### Fixed
- Free external stacks and task buffers through an idle-hook-driven deferred queue so PSRAM resources are only released after worker tasks fully unwind.

### Documentation
- Expanded the README with recipes for waiting on jobs, spawning function jobs, PSRAM usage, configuration, and error handling guidance.

## [1.0.1] - 2025-09-16

### Added
- Introduced the `examples/basic_lambda_worker` sketch covering minimal worker setup and join flow.

### Changed
- Simplified `examples/basic_worker` and `examples/psram_stack` to highlight the core worker and PSRAM patterns.

### Fixed
- Prevent workers from freeing their PSRAM-backed stacks while they finalize themselves by deferring cleanup until after `vTaskDelete` completes.

### Documentation
- Documented the wider ESPToolKit library set and polished README diagnostics formatting.

## [1.0.0] - 2025-09-16
### Added
- Initial ESPWorker implementation with configurable worker spawning, event/error callbacks, diagnostics, and PSRAM stack support.
- Example sketches demonstrating basic usage and PSRAM-backed workers.
- Continuous integration builds for PlatformIO and Arduino CLI across key ESP32 boards.
- Automated release packaging with PlatformIO artifact generation.

### Documentation
- Example-driven README with quick start, configuration tables, and troubleshooting notes.
