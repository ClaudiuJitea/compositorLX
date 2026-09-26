cmake_minimum_required(VERSION 3.24)

find_package(Qt6 REQUIRED COMPONENTS Test)

add_executable(compositor_tests
    TestProjectFormat.cpp
)

target_include_directories(compositor_tests PRIVATE ../src ../vendor/compositor-rendering)
target_link_libraries(compositor_tests PRIVATE compositor_ui Qt6::Test)
target_compile_definitions(compositor_tests PRIVATE
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

add_test(NAME compositor_tests COMMAND compositor_tests -platform offscreen)

add_executable(camera_raw_tests
    TestCameraRaw.cpp
)
target_include_directories(camera_raw_tests PRIVATE ../src ../vendor/compositor-rendering)
target_link_libraries(camera_raw_tests PRIVATE compositor_ui Qt6::Test)
target_compile_definitions(camera_raw_tests PRIVATE
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
add_test(NAME camera_raw_tests COMMAND camera_raw_tests -platform offscreen)

add_executable(svg_tests
    TestSvgImport.cpp
)
target_include_directories(svg_tests PRIVATE ../src ../vendor/compositor-rendering)
target_link_libraries(svg_tests PRIVATE compositor_ui Qt6::Test)
target_compile_definitions(svg_tests PRIVATE
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
add_test(NAME svg_tests COMMAND svg_tests -platform offscreen)

