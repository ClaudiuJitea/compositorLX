cmake_minimum_required(VERSION 3.24)

project(CompositorLX VERSION 0.7.6 LANGUAGES C CXX)

include(GNUInstallDirs)
include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Concurrent Network)
find_package(ZLIB REQUIRED)
find_package(Qt6 QUIET COMPONENTS OpenGLWidgets)
option(COMPOSITOR_ENABLE_VULKAN "Build optional Vulkan rendering acceleration" ON)
if(COMPOSITOR_ENABLE_VULKAN)
    find_path(COMPOSITOR_VULKAN_INCLUDE_DIR vulkan/vulkan.h)
endif()
find_path(POPPLER_QT6_INCLUDE_DIR poppler-qt6.h PATH_SUFFIXES poppler/qt6 REQUIRED)
find_library(POPPLER_QT6_LIBRARY NAMES poppler-qt6 REQUIRED)
find_path(GIF_INCLUDE_DIR gif_lib.h REQUIRED)
find_library(GIF_LIBRARY NAMES gif libgif.so.7 REQUIRED)
find_library(LCMS2_LIBRARY NAMES lcms2 REQUIRED)
find_path(JPEG_INCLUDE_DIR jpeglib.h REQUIRED)
find_path(LENSFUN_INCLUDE_DIR lensfun.h PATH_SUFFIXES lensfun REQUIRED)
find_library(LENSFUN_LIBRARY NAMES lensfun REQUIRED)
find_path(LENSFUN_DATA_DIR slr-canon.xml PATHS /usr/share/lensfun/version_1 /usr/local/share/lensfun/version_1)
find_path(TIFF_INCLUDE_DIR tiffio.h PATH_SUFFIXES x86_64-linux-gnu REQUIRED)
find_library(JPEG_LIBRARY NAMES jpeg libjpeg.so.8 REQUIRED)
find_library(TIFF_LIBRARY NAMES tiff libtiff.so.6 REQUIRED)
find_path(OPENEXR_INCLUDE_DIR OpenEXR/ImfRgbaFile.h REQUIRED)
find_path(IMATH_INCLUDE_DIR Imath/half.h REQUIRED)
find_library(OPENEXR_LIBRARY NAMES OpenEXR OpenEXR-3_1 libOpenEXR-3_1.so.30 REQUIRED)
find_library(IMATH_LIBRARY NAMES Imath Imath-3_1 libImath-3_1.so.29 REQUIRED)
find_package(PkgConfig QUIET)
find_package(libheif CONFIG QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBHEIF QUIET IMPORTED_TARGET libheif)
endif()
if(NOT LIBHEIF_FOUND)
    find_path(LIBHEIF_INCLUDE_DIR libheif/heif.h)
    find_library(LIBHEIF_LIBRARY NAMES heif libheif.so.1)
endif()
if(NOT TARGET heif AND NOT LIBHEIF_FOUND AND NOT (LIBHEIF_INCLUDE_DIR AND LIBHEIF_LIBRARY))
    message(FATAL_ERROR "libheif development files are required for HEIC/HEIF import")
endif()

find_library(LIBRAW_LIBRARY NAMES raw raw_r libraw.so.23 libraw_r.so.23 PATHS /usr/lib/x86_64-linux-gnu /usr/lib REQUIRED)
if(NOT LIBRAW_LIBRARY)
    message(WARNING "libraw was not found; RAW image import will be disabled")
endif()

# Foreground segmentation is local and deterministic.  Pin both the runtime
# and the small U2Net model so packaged builds never depend on a web service.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    # The archive may be given as a local copy (ONNXRUNTIME_ARCHIVE) when the release download is unavailable; the hash
    # below is checked either way.
    set(ONNXRUNTIME_ARCHIVE "https://github.com/microsoft/onnxruntime/releases/download/v1.23.2/onnxruntime-linux-x64-1.23.2.tgz"
        CACHE STRING "ONNX Runtime 1.23.2 linux-x64 archive (URL or local path)")
    FetchContent_Declare(onnxruntime
        URL "${ONNXRUNTIME_ARCHIVE}"
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
# Volumetric Light's depth model: macOS Compositor's DepthMini, converted to ONNX (models/DepthMini-NOTICE.txt).
set(DEPTH_MODEL "${SUBJECT_MODEL_DIR}/depthmini.onnx")
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/models/depthmini.onnx" DEPTH_MODEL_SOURCE_HASH)
if(NOT DEPTH_MODEL_SOURCE_HASH STREQUAL "e157ef8daf4d9912c3a878f384d29a0518e41ea58f27958b1ecc0beee4c4bd84")
    message(FATAL_ERROR "models/depthmini.onnx does not match its pinned SHA-256")
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/models/depthmini.onnx" "${DEPTH_MODEL}" COPYONLY)
set(SUBJECT_MODEL_LICENSE "${SUBJECT_MODEL_DIR}/U2Net-APACHE-2.0.txt")
if(NOT EXISTS "${SUBJECT_MODEL_LICENSE}")
    file(DOWNLOAD https://raw.githubusercontent.com/xuebinqin/U-2-Net/master/LICENSE "${SUBJECT_MODEL_LICENSE}"
        EXPECTED_HASH SHA256=c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4 TLS_VERIFY ON)
endif()

add_library(compositor_core
    src/camera_raw.qrc
    src/core/CameraRawTables.cpp
    vendor/compositor-rendering/PatchPixels.c
    vendor/compositor-rendering/DeepPixels.c
    src/core/CameraRaw.cpp
    src/core/CameraRaw.h
    src/core/Document.cpp
    src/core/Document.h
    src/core/DocumentHistory.cpp
    src/core/DocumentHistory.h
    src/core/EditorSessionGif.cpp
    src/io/ChannelPack.cpp
    src/io/GifIO.cpp
    src/io/MovieDecoder.cpp
    src/io/MovieDecoder.h
    src/core/EditorSessionMovie.cpp
    src/core/EditorSession.cpp
    src/core/EditorSessionLayers.cpp
    src/core/ShapeEditing.cpp
    src/core/LayerDistances.cpp
    src/core/LayerAlign.cpp
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
    src/core/DocumentLimits.h
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
    src/io/PrintExporter.cpp
    src/io/CMYK.cpp
    src/io/HdrImage.cpp
    src/io/PdfImporter.cpp
    src/io/ImageImporter.cpp
    src/io/ImageImporter.h
    src/io/PSDReader.cpp
    src/io/PSDWriter.cpp
    src/io/PSDText.cpp
    src/io/PSDText.h
    src/io/PSDVector.cpp
    src/io/PSDVector.h
    src/io/PSDReader.h
    src/io/LensCorrection.cpp
    src/io/LensCorrection.h
    src/io/RawImporter.cpp
    src/io/RawImporter.h
    src/io/SvgImporter.cpp
    src/io/SvgImporter.h
    src/rendering/LayerRenderer.cpp
    src/rendering/LayerRenderer.h
    src/rendering/LayerEffectsRenderer.cpp
    src/rendering/LayerEffectsRenderer.h
    src/rendering/GpuDocument.cpp
    src/rendering/GpuRenderer.cpp
    src/rendering/GpuVulkan.cpp
    src/rendering/GpuOperations.cpp
    src/rendering/RendererSelfCheck.cpp
    src/rendering/RgbSplitVulkan.cpp
    src/rendering/RgbSplit.cpp
    src/rendering/LensEffects.cpp
    src/rendering/FilterLayers.cpp
    src/rendering/RimLight.cpp
    src/rendering/VolumetricLight.cpp
    src/rendering/LensEffects.h
    src/rendering/ProgressiveBlur.cpp
    src/rendering/DeepImage.cpp
    src/rendering/Scanlines.cpp
    src/rendering/Dither.cpp
    src/rendering/Dither.h
    src/rendering/DownsampleCache.cpp
    src/rendering/DownsampleCache.h
    src/rendering/RasterOperations.cpp
    src/rendering/RasterOperations.h
    src/rendering/SubjectRemoval.cpp
    src/rendering/SubjectRemoval.h
    src/rendering/TextFaces.cpp
    src/rendering/TextFaces.h
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
target_include_directories(compositor_core PUBLIC src vendor/compositor-rendering PRIVATE ${POPPLER_QT6_INCLUDE_DIR} ${GIF_INCLUDE_DIR} ${LENSFUN_INCLUDE_DIR} ${OPENEXR_INCLUDE_DIR} ${IMATH_INCLUDE_DIR} ${IMATH_INCLUDE_DIR}/Imath ${JPEG_INCLUDE_DIR} ${TIFF_INCLUDE_DIR})
target_link_libraries(compositor_core PRIVATE ${POPPLER_QT6_LIBRARY} ${GIF_LIBRARY} ${LENSFUN_LIBRARY} ${OPENEXR_LIBRARY} ${IMATH_LIBRARY} ${LCMS2_LIBRARY} ${JPEG_LIBRARY} ${TIFF_LIBRARY})
if(LENSFUN_DATA_DIR)
    target_compile_definitions(compositor_core PRIVATE COMPOSITOR_LENSFUN_DATA_DIR="${LENSFUN_DATA_DIR}")
endif()
find_package(OpenMP COMPONENTS C CXX)
if(OpenMP_C_FOUND)
    target_link_libraries(compositor_core PRIVATE OpenMP::OpenMP_C OpenMP::OpenMP_CXX)
endif()
target_link_libraries(compositor_core PUBLIC Qt6::Core Qt6::Gui Qt6::Concurrent ZLIB::ZLIB)
if(COMPOSITOR_ENABLE_VULKAN AND COMPOSITOR_VULKAN_INCLUDE_DIR)
    target_include_directories(compositor_core PRIVATE "${COMPOSITOR_VULKAN_INCLUDE_DIR}")
    target_compile_definitions(compositor_core PRIVATE COMPOSITOR_HAVE_VULKAN=1)
    message(STATUS "Vulkan renderer: enabled (runtime loader, CPU fallback)")
else()
    message(STATUS "Vulkan renderer: disabled (CPU fallback)")
endif()
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
# Movie layers read video through FFmpeg (mac AVFoundation). The versioned names find the runtime libraries when the
# development symlinks aren't installed system-wide (the headers then come from CMAKE_PREFIX_PATH).
find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h PATH_SUFFIXES x86_64-linux-gnu ffmpeg)
foreach(component avformat:62 avcodec:62 avutil:60 swscale:9)
    string(REPLACE ":" ";" parts "${component}")
    list(GET parts 0 name)
    list(GET parts 1 major)
    string(TOUPPER "${name}" upper)
    find_library(FFMPEG_${upper}_LIBRARY NAMES lib${name}.so.${major} lib${name}.so ${name})
    if(NOT FFMPEG_${upper}_LIBRARY)
        message(FATAL_ERROR "FFmpeg's lib${name} is required for movie layers (apt install lib${name}-dev)")
    endif()
    list(APPEND FFMPEG_LIBRARIES "${FFMPEG_${upper}_LIBRARY}")
endforeach()
if(NOT FFMPEG_INCLUDE_DIR)
    message(FATAL_ERROR "FFmpeg's headers are required for movie layers (apt install libavformat-dev libavcodec-dev libswscale-dev)")
endif()
target_include_directories(compositor_core PRIVATE "${FFMPEG_INCLUDE_DIR}")
target_link_libraries(compositor_core PRIVATE ${FFMPEG_LIBRARIES})
target_compile_options(compositor_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(compositor_ui STATIC
    src/resources.qrc
    src/ui/GpuViewport.cpp
    src/ui/AsyncImagePreview.cpp
    src/ui/RenderingSettingsDialog.cpp
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
    src/ui/MainWindowPort.cpp
    src/ui/GifTimeline.cpp
    src/ui/MovieTimeline.cpp
    src/ui/MovieTimeline.h
    src/ui/MainWindowFilterLayers.cpp
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
# A movie layer's sound plays through Qt Multimedia (mac AVPlayer); without it, movies play silently.
# Qt looks for its modules beside its own; looked for by name, Multimedia may come from CMAKE_PREFIX_PATH too.
find_package(Qt6Multimedia QUIET)
if(TARGET Qt6::Multimedia)
    target_link_libraries(compositor_ui PRIVATE Qt6::Multimedia)
    target_compile_definitions(compositor_ui PRIVATE COMPOSITOR_HAVE_MULTIMEDIA=1)
    # Qt finds its FFmpeg media backend among the system's plugins; a Multimedia from CMAKE_PREFIX_PATH has it beside
    # itself, which development builds add to the search path.
    get_filename_component(COMPOSITOR_MULTIMEDIA_PLUGINS "${Qt6Multimedia_DIR}/../../qt6/plugins" ABSOLUTE)
    if(EXISTS "${COMPOSITOR_MULTIMEDIA_PLUGINS}/multimedia")
        target_compile_definitions(compositor_ui PRIVATE COMPOSITOR_DEV_PLUGIN_DIR="${COMPOSITOR_MULTIMEDIA_PLUGINS}")
    endif()
    message(STATUS "Movie sound: Qt Multimedia ${Qt6Multimedia_VERSION}")
else()
    message(STATUS "Movie sound: off (Qt Multimedia not found)")
endif()
if(TARGET Qt6::OpenGLWidgets)
    target_link_libraries(compositor_ui PRIVATE Qt6::OpenGLWidgets)
    target_compile_definitions(compositor_ui PRIVATE COMPOSITOR_GPU_CANVAS=1)
endif()

add_executable(compositor-lx
    src/main.cpp
)
target_link_libraries(compositor-lx PRIVATE compositor_ui)
target_compile_definitions(compositor-lx PRIVATE COMPOSITOR_VERSION="${PROJECT_VERSION}")
target_compile_options(compositor-lx PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_subdirectory(tests)

install(TARGETS compositor-lx RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(ONNXRUNTIME_ROOT)
    install(FILES "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so.1.23.2"
            DESTINATION ${CMAKE_INSTALL_LIBDIR} RENAME libonnxruntime.so.1)
    set_target_properties(compositor-lx PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
install(FILES "${SUBJECT_MODEL}" "${DEPTH_MODEL}" DESTINATION ${CMAKE_INSTALL_DATADIR}/compositor-lx/models)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/models/DepthMini-NOTICE.txt" DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/compositor-lx)
install(FILES "${SUBJECT_MODEL_LICENSE}" DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/compositor-lx)
if(ONNXRUNTIME_ROOT)
    install(FILES "${ONNXRUNTIME_ROOT}/LICENSE" DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/compositor-lx RENAME ONNXRuntime-MIT.txt)
endif()
install(DIRECTORY packaging/licenses/ DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/compositor-lx/licenses)
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
# Qt Multimedia may come from CMAKE_PREFIX_PATH rather than an installed package, which dpkg-shlibdeps can't name: its
# directory is private to the build, and the package is named here (movie sound; FFmpeg's libraries are found as usual).
if(TARGET Qt6::Multimedia)
    get_target_property(COMPOSITOR_MULTIMEDIA_LIBRARY Qt6::Multimedia IMPORTED_LOCATION_RELEASE)
    if(NOT COMPOSITOR_MULTIMEDIA_LIBRARY)
        get_target_property(COMPOSITOR_MULTIMEDIA_LIBRARY Qt6::Multimedia LOCATION)
    endif()
    get_filename_component(COMPOSITOR_MULTIMEDIA_LIBDIR "${COMPOSITOR_MULTIMEDIA_LIBRARY}" DIRECTORY)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS "${COMPOSITOR_MULTIMEDIA_LIBDIR}")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6multimedia6 (>= 6.5)")
endif()
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
