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


add_executable(text_runs_tests
    TestTextRuns.cpp
)
target_include_directories(text_runs_tests PRIVATE ../src ../vendor/compositor-rendering)
target_link_libraries(text_runs_tests PRIVATE compositor_ui Qt6::Test)
target_compile_definitions(text_runs_tests PRIVATE
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
add_test(NAME text_runs_tests COMMAND text_runs_tests -platform offscreen)

add_executable(text_editing_tests
    TestTextEditing.cpp
)
target_include_directories(text_editing_tests PRIVATE ../src ../vendor/compositor-rendering)
target_link_libraries(text_editing_tests PRIVATE compositor_ui Qt6::Test)
target_compile_definitions(text_editing_tests PRIVATE
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
add_test(NAME text_editing_tests COMMAND text_editing_tests -platform offscreen)

add_executable(sync_rendering_tests
    TestSyncRendering.cpp
)
target_include_directories(sync_rendering_tests PRIVATE ../src ../vendor/compositor-rendering)
target_link_libraries(sync_rendering_tests PRIVATE compositor_ui Qt6::Test)
target_compile_definitions(sync_rendering_tests PRIVATE
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
add_test(NAME sync_rendering_tests COMMAND sync_rendering_tests -platform offscreen)

add_executable(dither_tests
    TestDither.cpp
)
target_include_directories(dither_tests PRIVATE ../src ../vendor/compositor-rendering)
target_link_libraries(dither_tests PRIVATE compositor_ui Qt6::Test)
add_test(NAME dither_tests COMMAND dither_tests -platform offscreen)
add_executable(paint_tools_tests
    TestPaintTools.cpp
)
target_include_directories(paint_tools_tests PRIVATE ../src ../vendor/compositor-rendering)
target_link_libraries(paint_tools_tests PRIVATE compositor_ui Qt6::Test)
add_test(NAME paint_tools_tests COMMAND paint_tools_tests -platform offscreen)

add_executable(canvas_ops_tests
    TestCanvasOps.cpp
)
target_include_directories(canvas_ops_tests PRIVATE ../src ../vendor/compositor-rendering)
target_link_libraries(canvas_ops_tests PRIVATE compositor_ui Qt6::Test)
add_test(NAME canvas_ops_tests COMMAND canvas_ops_tests -platform offscreen)
add_executable(selection_tests
    TestSelection.cpp
)
target_include_directories(selection_tests PRIVATE ../src ../vendor/compositor-rendering)
target_link_libraries(selection_tests PRIVATE compositor_ui Qt6::Test)
add_test(NAME selection_tests COMMAND selection_tests -platform offscreen)

add_executable(selection_canvas_tests
    TestSelectionCanvas.cpp
)
target_include_directories(selection_canvas_tests PRIVATE ../src ../vendor/compositor-rendering)
target_link_libraries(selection_canvas_tests PRIVATE compositor_ui Qt6::Test)
add_test(NAME selection_canvas_tests COMMAND selection_canvas_tests -platform offscreen)
add_executable(filters_tests
    TestFilters.cpp
)
target_include_directories(filters_tests PRIVATE ../src ../vendor/compositor-rendering)
target_link_libraries(filters_tests PRIVATE compositor_ui Qt6::Test)
target_compile_definitions(filters_tests PRIVATE
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
add_test(NAME filters_tests COMMAND filters_tests -platform offscreen)
