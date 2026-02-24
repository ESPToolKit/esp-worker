add_library(esp_worker_core STATIC
    ${PROJECT_SOURCE_DIR}/src/esp_worker/worker.cpp
)

target_include_directories(esp_worker_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)

target_compile_features(esp_worker_core PUBLIC cxx_std_17)

add_executable(esp_worker_lifecycle_tests
    esp_worker_lifecycle_tests.cpp
    worker_test_stubs.cpp
)

target_include_directories(esp_worker_lifecycle_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)

target_link_libraries(esp_worker_lifecycle_tests
    PRIVATE
        esp_worker_core
)

target_compile_features(esp_worker_lifecycle_tests PRIVATE cxx_std_17)

add_test(NAME esp_worker_lifecycle_tests COMMAND esp_worker_lifecycle_tests)
