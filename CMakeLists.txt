cmake_minimum_required(VERSION 3.24)

project(CompositorLX VERSION 0.4.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Concurrent Network)
find_package(ZLIB REQUIRED)
find_package(PkgConfig QUIET)
find_package(libheif CONFIG QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBHEIF QUIET IMPORTED_TARGET libheif)
endif()
if(NOT LIBHEIF_FOUND)
    find_path(LIBHEIF_INCLUDE_DIR libheif/heif.h)
    find_library(LIBHEIF_LIBRARY heif)
endif()
if(NOT TARGET heif AND NOT LIBHEIF_FOUND AND NOT (LIBHEIF_INCLUDE_DIR AND LIBHEIF_LIBRARY))
    message(WARNING "libheif development files were not found; HEIC/HEIF import will rely on an installed Qt image plugin")
endif()

find_library(LIBRAW_LIBRARY NAMES raw raw_r libraw.so.23 libraw_r.so.23 PATHS /usr/lib/x86_64-linux-gnu /usr/lib)
if(NOT LIBRAW_LIBRARY)
    message(WARNING "libraw was not found; RAW image import will be disabled")
endif()

# Foreground segmentation is local and deterministic.  Pin both the runtime
# and the small U2Net model so packaged builds never depend on a web service.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    FetchContent_Declare(onnxruntime
        URL https://github.com/microsoft/onnxruntime/releases/download/v1.23.2/onnxruntime-linux-x64-1.23.2.tgz
        URL_HASH SHA256=1fa4dcaef22f6f7d5cd81b28c2800414350c10116f5fdd46a2160082551c5f9b)
    FetchContent_MakeAvailable(onnxruntime)
    set(ONNXRUNTIME_ROOT "${onnxruntime_SOURCE_DIR}")
else()
    find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h PATH_SUFFIXES onnxruntime)
    find_library(ONNXRUNTIME_LIBRARY onnxruntime)
endif()

set(SUBJECT_MODEL_DIR "${CMAKE_BINARY_DIR}/models")
set(SUBJECT_MODEL "${SUBJECT_MODEL_DIR}/u2netp.onnx")
file(MAKE_DIRECTORY "${SUBJECT_MODEL_DIR}")
set(SUBJECT_MODEL_HASH "309c8469258dda742793dce0ebea8e6dd393174f89934733ecc8b14c76f4ddd8")
if(EXISTS "${SUBJECT_MODEL}")
    file(SHA256 "${SUBJECT_MODEL}" SUBJECT_MODEL_EXISTING_HASH)
endif()
if(NOT SUBJECT_MODEL_EXISTING_HASH STREQUAL SUBJECT_MODEL_HASH)
    file(REMOVE "${SUBJECT_MODEL}")
    file(DOWNLOAD https://huggingface.co/edgetools/u2netp/resolve/main/u2netp.onnx "${SUBJECT_MODEL}"
        EXPECTED_HASH SHA256=${SUBJECT_MODEL_HASH}
        TLS_VERIFY ON SHOW_PROGRESS STATUS SUBJECT_MODEL_DOWNLOAD)
    list(GET SUBJECT_MODEL_DOWNLOAD 0 SUBJECT_MODEL_STATUS)
    if(NOT SUBJECT_MODEL_STATUS EQUAL 0)
        list(GET SUBJECT_MODEL_DOWNLOAD 1 SUBJECT_MODEL_ERROR)
        message(FATAL_ERROR "Could not download the pinned U2Net model: ${SUBJECT_MODEL_ERROR}")
    endif()
endif()
set(SUBJECT_MODEL_LICENSE "${SUBJECT_MODEL_DIR}/U2Net-APACHE-2.0.txt")
if(NOT EXISTS "${SUBJECT_MODEL_LICENSE}")
    file(DOWNLOAD https://raw.githubusercontent.com/xuebinqin/U-2-Net/master/LICENSE "${SUBJECT_MODEL_LICENSE}"
        EXPECTED_HASH SHA256=c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4 TLS_VERIFY ON)
endif()

add_library(compositor_core
    src/core/CameraRaw.cpp
    src/core/CameraRaw.h
    src/core/Document.cpp
    src/core/Document.h
    src/core/DocumentHistory.cpp
    src/core/DocumentHistory.h
    src/core/EditorSession.cpp
    src/core/EditorSessionLayers.cpp
    src/core/EditorSession.h
    src/core/ImageTrim.cpp
    src/core/ImageTrim.h
    src/core/SelectionOps.cpp
    src/core/SelectionOps.h
    src/io/AdjustmentJson.cpp
    src/io/AdjustmentJson.h
    src/core/ColorPalette.cpp
    src/core/ColorPalette.h
    src/core/EditorSessionPaint.cpp
    src/io/ProjectReader.cpp
    src/io/ProjectReader.h
    src/io/ProjectWriter.cpp
    src/io/ProjectWriter.h
    src/io/ProjectDigest.cpp
    src/io/ProjectDigest.h
    src/io/ProjectWatcher.cpp
    src/io/ProjectWatcher.h
    src/io/ImageExporter.cpp
    src/io/ImageExporter.h
    src/io/ImageImporter.cpp
    src/io/ImageImporter.h
    src/io/PSDReader.cpp
    src/io/PSDReader.h
    src/io/RawImporter.cpp
    src/io/RawImporter.h
    src/io/SvgImporter.cpp
    src/io/SvgImporter.h
    src/rendering/LayerRenderer.cpp
    src/rendering/LayerRenderer.h
    src/rendering/LayerEffectsRenderer.cpp
    src/rendering/LayerEffectsRenderer.h
    src/rendering/Dither.cpp
    src/rendering/Dither.h
    src/rendering/DownsampleCache.cpp
    src/rendering/DownsampleCache.h
    src/rendering/RasterOperations.cpp
    src/rendering/RasterOperations.h
    src/rendering/SubjectRemoval.cpp
    src/rendering/SubjectRemoval.h
    vendor/compositor-rendering/AdjustPixels.c
    vendor/compositor-rendering/BrushPixels.c
    vendor/compositor-rendering/ContentFill.c
    vendor/compositor-rendering/DitherPixels.c
    vendor/compositor-rendering/HealPixels.c
    vendor/compositor-rendering/LensPixels.c
    vendor/compositor-rendering/LevelsPixels.c
    vendor/compositor-rendering/NoisePixels.c
    vendor/compositor-rendering/WandPixels.c
)
target_include_directories(compositor_core PUBLIC src vendor/compositor-rendering)
find_package(OpenMP COMPONENTS C)
if(OpenMP_C_FOUND)
    target_link_libraries(compositor_core PRIVATE OpenMP::OpenMP_C)
endif()
target_link_libraries(compositor_core PUBLIC Qt6::Core Qt6::Gui Qt6::Concurrent ZLIB::ZLIB)
if(TARGET heif OR LIBHEIF_FOUND OR (LIBHEIF_INCLUDE_DIR AND LIBHEIF_LIBRARY))
    if(TARGET heif)
        target_link_libraries(compositor_core PRIVATE heif)
    elseif(LIBHEIF_FOUND)
        target_link_libraries(compositor_core PRIVATE PkgConfig::LIBHEIF)
    else()
        target_include_directories(compositor_core PRIVATE "${LIBHEIF_INCLUDE_DIR}")
        target_link_libraries(compositor_core PRIVATE "${LIBHEIF_LIBRARY}")
    endif()
    target_compile_definitions(compositor_core PRIVATE COMPOSITOR_HAVE_LIBHEIF=1)
endif()
if(ONNXRUNTIME_ROOT)
    target_include_directories(compositor_core PRIVATE "${ONNXRUNTIME_ROOT}/include")
    target_link_libraries(compositor_core PRIVATE "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so")
elseif(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
    target_include_directories(compositor_core PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}")
    target_link_libraries(compositor_core PRIVATE "${ONNXRUNTIME_LIBRARY}")
else()
    message(FATAL_ERROR "ONNX Runtime is required for Remove Background")
endif()
if(LIBRAW_LIBRARY)
    target_include_directories(compositor_core PUBLIC vendor/libraw/include)
    target_link_libraries(compositor_core PRIVATE "${LIBRAW_LIBRARY}")
    target_compile_definitions(compositor_core PUBLIC COMPOSITOR_HAVE_LIBRAW=1)
endif()
target_compile_options(compositor_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(compositor_ui STATIC
    src/resources.qrc
    src/ui/CameraRawDialog.cpp
    src/ui/CameraRawDialog.h
    src/ui/CanvasRulerWidget.cpp
    src/ui/CanvasRulerWidget.h
    src/ui/CanvasWidget.cpp
    src/ui/ColorPickerDialog.cpp
    src/ui/ColorPickerDialog.h
    src/ui/CanvasWidget.h
    src/ui/EffectsDialog.cpp
    src/ui/EffectsDialog.h
    src/ui/InlineTextEditor.h
    src/ui/KeyboardShortcutsDialog.cpp
    src/ui/KeyboardShortcutsDialog.h
    src/ui/LayerListModel.cpp
    src/ui/LayerListModel.h
    src/ui/MainWindow.cpp
    src/ui/MainWindowLayerMenu.cpp
    src/ui/MainWindow.h
    src/ui/NumericScrub.cpp
    src/ui/NumericScrub.h
    src/ui/RawDevelopDialog.cpp
    src/ui/RawDevelopDialog.h
    src/ui/SegmentedControl.h
    src/ui/ShortcutManager.cpp
    src/ui/ShortcutManager.h
    src/ui/TrimDialog.cpp
    src/ui/TrimDialog.h
)
target_include_directories(compositor_ui PUBLIC src vendor/compositor-rendering)
target_link_libraries(compositor_ui PUBLIC compositor_core Qt6::Widgets Qt6::Concurrent Qt6::Network)
target_compile_options(compositor_ui PRIVATE -Wall -Wextra -Wpedantic)

add_executable(compositor-lx
    src/main.cpp
)
target_link_libraries(compositor-lx PRIVATE compositor_ui)
target_compile_options(compositor-lx PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_subdirectory(tests)

install(TARGETS compositor-lx RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(ONNXRUNTIME_ROOT)
    install(FILES "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so.1.23.2"
            DESTINATION ${CMAKE_INSTALL_LIBDIR} RENAME libonnxruntime.so.1)
    set_target_properties(compositor-lx PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
install(FILES "${SUBJECT_MODEL}" DESTINATION ${CMAKE_INSTALL_DATADIR}/compositor-lx/models)
install(FILES "${SUBJECT_MODEL_LICENSE}" DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/compositor-lx)
if(ONNXRUNTIME_ROOT)
    install(FILES "${ONNXRUNTIME_ROOT}/LICENSE" DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/compositor-lx RENAME ONNXRuntime-MIT.txt)
endif()
install(FILES packaging/compositor-lx.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/compositor-lx.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES packaging/icons/compositor-lx.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
foreach(icon_size 16 32 48 64 128 256 512 1024)
    install(FILES packaging/icons/compositor-lx-${icon_size}.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps
            RENAME compositor-lx.png)
endforeach()

set(CPACK_PACKAGE_NAME "CompositorLX")
set(CPACK_PACKAGE_VENDOR "Compositor")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Layer-based raster image compositor")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "Compositor maintainers")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ;DEB")
set(CPACK_DEBIAN_PACKAGE_SECTION "graphics")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
