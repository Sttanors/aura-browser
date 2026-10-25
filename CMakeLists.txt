cmake_minimum_required(VERSION 3.31)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/common/update_key.hpp" AURA_IDENTITY_LINE REGEX "^inline constexpr std::string_view browser_version")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" AURA_PROJECT_VERSION "${AURA_IDENTITY_LINE}")
project(Aura2 VERSION ${AURA_PROJECT_VERSION} LANGUAGES C CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS common/update_key.hpp)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(AURA_BUILD_DIAGNOSTIC "Build the CEF + Skia M0 executable" ON)
if(WIN32)
  set(AURA_COMPATIBILITY_DEFAULT OFF)
else()
  set(AURA_COMPATIBILITY_DEFAULT ON)
endif()
option(AURA_BUILD_COMPATIBILITY_HARNESS "Build the legacy M0 diagnostic harness" ${AURA_COMPATIBILITY_DEFAULT})
include(CTest)
find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
include(cmake/dependencies.cmake)
find_package(OpenSSL REQUIRED)

set(AURA_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
  OUTPUT "${AURA_GENERATED}/aura/design_tokens.hpp"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_tokens.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/design/tokens.json"
          "${AURA_GENERATED}/aura/design_tokens.hpp"
  DEPENDS design/tokens.json scripts/generate_tokens.py VERBATIM)
add_custom_target(aura_design_tokens DEPENDS "${AURA_GENERATED}/aura/design_tokens.hpp")
add_library(aura_common core/browser/duplicate_tabs.cpp core/browser/search_providers.cpp core/browser/reading_list.cpp core/browser/process_tasks.cpp core/browser/download_hash.cpp core/browser/container_rules.cpp core/browser/site_zoom.cpp common/exports.cpp core/browser/screenshot.cpp core/browser/hotkeys.cpp core/browser/sessions.cpp core/browser/bookmarks.cpp core/browser/permissions.cpp core/browser/translation.cpp common/file_lock.cpp core/browser/profiles.cpp common/platform.cpp common/config.cpp common/paint_buffer.cpp common/pump_schedule.cpp core/browser/profile.cpp core/browser/i18n.cpp core/browser/preferences.cpp core/browser/update_policy.cpp core/browser/passwords.cpp)
target_include_directories(aura_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${AURA_GENERATED}")
target_link_libraries(aura_common PUBLIC tomlplusplus::tomlplusplus spdlog::spdlog OpenSSL::Crypto)
add_dependencies(aura_common aura_design_tokens)
if(MSVC)
  target_compile_options(aura_common PUBLIC /utf-8 /W4 /EHsc)
  target_compile_definitions(aura_common PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
  target_compile_options(aura_common PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
  add_executable(aura_tests tests/common_tests.cpp tests/update_tests.cpp tests/password_tests.cpp)
  target_link_libraries(aura_tests PRIVATE aura_common GTest::gtest_main)
  target_compile_definitions(aura_tests PRIVATE AURA_TEST_PYTHON="${Python3_EXECUTABLE}")
  include(GoogleTest)
  gtest_discover_tests(aura_tests)
  add_test(NAME bootstrap_validation COMMAND "${Python3_EXECUTABLE}" -m unittest discover
           -s "${CMAKE_CURRENT_SOURCE_DIR}/tests/python" -v)
endif()

if(AURA_BUILD_DIAGNOSTIC)
  include(cmake/cef.cmake)
  include(cmake/skia.cmake)
  if(AURA_BUILD_COMPATIBILITY_HARNESS)
  set(AURA_SOURCES app/main.cpp core/cef/diagnostic_client.cpp render/diagnostic_scene.cpp)
  if(WIN32)
    list(APPEND AURA_SOURCES platform/windows/diagnostic_window.cpp
         render/rhi/angle_surface.cpp render/cef_bridge/shared_texture_probe.cpp
         platform/windows/aura.manifest)
    add_library(aura_diagnostic SHARED ${AURA_SOURCES})
    target_compile_definitions(aura_diagnostic PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(aura_diagnostic PRIVATE dwmapi d3d11 dxgi imm32 shcore opengl32)
    target_link_options(aura_diagnostic PRIVATE /DELAYLOAD:libcef.dll ${CEF_DELAYLOAD_FLAGS})
    target_link_libraries(aura_diagnostic PRIVATE ${CEF_STANDARD_LIBS})
    set_target_properties(aura_diagnostic PROPERTIES PREFIX "")
  else()
    find_package(X11 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(AURA_EGL REQUIRED IMPORTED_TARGET egl)
    pkg_check_modules(AURA_GLES REQUIRED IMPORTED_TARGET glesv2)
    list(APPEND AURA_SOURCES platform/linux/x11/diagnostic_window.cpp)
    add_executable(aura_diagnostic ${AURA_SOURCES})
    set_target_properties(aura_diagnostic PROPERTIES BUILD_RPATH "$ORIGIN")
    target_link_libraries(aura_diagnostic PRIVATE dl pthread X11::X11 PkgConfig::AURA_EGL PkgConfig::AURA_GLES)
  endif()
  target_link_libraries(aura_diagnostic PRIVATE aura_common aura_skia libcef_lib libcef_dll_wrapper)
  target_include_directories(aura_diagnostic PRIVATE "${CEF_ROOT}")
  if(MSVC)
    target_compile_options(aura_diagnostic PRIVATE /EHsc /utf-8 /W4)
    target_compile_options(aura_common PUBLIC /EHsc)
  else()
    target_compile_options(aura_diagnostic PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
  endif()
  target_compile_definitions(aura_diagnostic PRIVATE ${CEF_COMPILER_DEFINES})
  # The CEF wrapper keeps its own no-exception policy. Aura uses exceptions internally.
  get_target_property(AURA_DEFINITIONS aura_diagnostic COMPILE_DEFINITIONS)
  list(REMOVE_ITEM AURA_DEFINITIONS _HAS_EXCEPTIONS=0)
  set_target_properties(aura_diagnostic PROPERTIES COMPILE_DEFINITIONS "${AURA_DEFINITIONS}")
  if(WIN32)
    target_compile_definitions(aura_diagnostic PRIVATE CEF_USE_BOOTSTRAP)
  endif()
  add_custom_command(TARGET aura_diagnostic POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CEF_ROOT}/Release" "$<TARGET_FILE_DIR:aura_diagnostic>"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CEF_ROOT}/Resources" "$<TARGET_FILE_DIR:aura_diagnostic>"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts"
            "$<TARGET_FILE_DIR:aura_diagnostic>/fonts"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/resources/default.toml"
            "$<TARGET_FILE_DIR:aura_diagnostic>/default.toml"
    VERBATIM)
  if(WIN32)
    add_custom_command(TARGET aura_diagnostic POST_BUILD
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/stage_windows.py"
              "$<TARGET_FILE_DIR:aura_diagnostic>" "${CEF_ROOT}"
              "${PROJECT_SOURCE_DIR}/third_party/skia-build/windows64" "${CMAKE_MT}"
      VERBATIM)
  endif()
endif()

endif()

# The normal desktop application. M0 remains a separate compatibility harness.
if(AURA_BUILD_DIAGNOSTIC AND NOT APPLE)
  find_package(OpenSSL REQUIRED)
  find_package(SQLite3 REQUIRED)
  if(TARGET SQLite3::SQLite3)
    set(AURA_SQLITE_TARGET SQLite3::SQLite3)
  else()
    set(AURA_SQLITE_TARGET SQLite::SQLite3)
  endif()
  if(WIN32)
    find_package(LibArchive REQUIRED)
    set(AURA_BROWSER_LIBS LibArchive::LibArchive user32 gdi32 dwmapi imm32 shcore shell32 comdlg32 ole32
      ntdll userenv ws2_32 advapi32 bcrypt crypt32)
    set(AURA_RUST_LIBRARY aura_adblock.lib)
    set(AURA_RUST_FLAGS "-C target-feature=+crt-static")
  else()
    pkg_check_modules(AURA_ARCHIVE REQUIRED IMPORTED_TARGET libarchive)
    pkg_check_modules(AURA_WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
    set(AURA_BROWSER_LIBS X11::X11 X11::Xext PkgConfig::AURA_WAYLAND PkgConfig::AURA_EGL
      PkgConfig::AURA_GLES dl pthread PkgConfig::AURA_ARCHIVE)
    set(AURA_RUST_LIBRARY libaura_adblock.a)
    set(AURA_RUST_FLAGS "")
  endif()
  find_program(AURA_CARGO cargo REQUIRED)
  set(AURA_RUST_TARGET "${PROJECT_SOURCE_DIR}/third_party/adblock-build")
  add_custom_command(OUTPUT "${AURA_RUST_TARGET}/release/${AURA_RUST_LIBRARY}"
    COMMAND "${CMAKE_COMMAND}" -E env "CARGO_HOME=${PROJECT_SOURCE_DIR}/third_party/cargo-home"
      "CARGO_TARGET_DIR=${AURA_RUST_TARGET}" "RUSTFLAGS=${AURA_RUST_FLAGS}" "${AURA_CARGO}" build
      --manifest-path "${PROJECT_SOURCE_DIR}/core/adblock/Cargo.toml" --release --locked --jobs 2
    DEPENDS core/adblock/src/lib.rs core/adblock/Cargo.toml core/adblock/Cargo.lock VERBATIM)
  add_custom_target(aura_adblock_build DEPENDS "${AURA_RUST_TARGET}/release/${AURA_RUST_LIBRARY}")
  add_library(aura_adblock STATIC IMPORTED)
  set_target_properties(aura_adblock PROPERTIES IMPORTED_LOCATION "${AURA_RUST_TARGET}/release/${AURA_RUST_LIBRARY}")
  add_dependencies(aura_adblock aura_adblock_build)
  set(AURA_BROWSER_SOURCES app/browser_main.cpp core/browser/browser.cpp core/browser/client.cpp core/browser/devtools.cpp core/browser/tab_lifecycle.cpp core/browser/split_view.cpp core/browser/containers.cpp core/browser/downloads.cpp core/browser/task_manager.cpp core/browser/merge_duplicates.cpp core/browser/protection.cpp core/browser/filter_lists.cpp core/browser/extensions.cpp core/browser/site_tools.cpp core/browser/extension_popup.cpp core/browser/updates.cpp core/browser/imports.cpp
    core/cef/diagnostic_client.cpp core/browser/capture_screenshot.cpp core/browser/site_data.cpp core/browser/site_permissions.cpp core/browser/reader.cpp ui/browser_ui.cpp ui/wallpaper.cpp)
  if(WIN32)
    add_library(aura_browser SHARED ${AURA_BROWSER_SOURCES} platform/windows/browser_window.cpp render/cef_bridge/shared_texture_probe.cpp)
    target_link_libraries(aura_browser PRIVATE d3d11 dxgi)
    target_compile_definitions(aura_browser PRIVATE CEF_USE_BOOTSTRAP)
    target_compile_options(aura_browser PRIVATE /EHsc /utf-8 /W4)
    target_link_options(aura_browser PRIVATE /DELAYLOAD:libcef.dll ${CEF_DELAYLOAD_FLAGS})
    target_link_libraries(aura_browser PRIVATE ${CEF_STANDARD_LIBS})
    set_target_properties(aura_browser PROPERTIES PREFIX "")
  else()
    add_executable(aura_browser ${AURA_BROWSER_SOURCES} platform/linux/x11/browser_window.cpp
      platform/linux/x11/software_presenter.cpp platform/linux/x11/wayland_refresh.cpp)
    target_compile_options(aura_browser PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    set_target_properties(aura_browser PROPERTIES BUILD_RPATH "$ORIGIN")
  endif()
  target_link_libraries(aura_browser PRIVATE aura_common aura_skia libcef_lib libcef_dll_wrapper
    ${AURA_BROWSER_LIBS} aura_adblock OpenSSL::Crypto ${AURA_SQLITE_TARGET})
  target_include_directories(aura_browser PRIVATE "${CEF_ROOT}")
  target_compile_definitions(aura_browser PRIVATE ${CEF_COMPILER_DEFINES})
  get_target_property(AURA_BROWSER_DEFINITIONS aura_browser COMPILE_DEFINITIONS)
  list(REMOVE_ITEM AURA_BROWSER_DEFINITIONS _HAS_EXCEPTIONS=0)
  set_target_properties(aura_browser PROPERTIES COMPILE_DEFINITIONS "${AURA_BROWSER_DEFINITIONS}")
  if(BUILD_TESTING)
    if(NOT WIN32)
      add_executable(aura_wayland_tests tests/wayland_refresh_tests.cpp platform/linux/x11/wayland_refresh.cpp)
      target_link_libraries(aura_wayland_tests PRIVATE aura_common PkgConfig::AURA_WAYLAND GTest::gtest_main)
      gtest_discover_tests(aura_wayland_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 3)
    endif()
    add_executable(aura_import_tests tests/import_tests.cpp core/browser/imports.cpp)
    target_include_directories(aura_import_tests PRIVATE "${CEF_ROOT}")
    target_compile_definitions(aura_import_tests PRIVATE ${CEF_COMPILER_DEFINES})
    target_link_libraries(aura_import_tests PRIVATE aura_common libcef_lib libcef_dll_wrapper
      ${AURA_BROWSER_LIBS} ${AURA_SQLITE_TARGET} GTest::gtest_main)
    get_target_property(AURA_IMPORT_TEST_DEFINITIONS aura_import_tests COMPILE_DEFINITIONS)
    list(REMOVE_ITEM AURA_IMPORT_TEST_DEFINITIONS _HAS_EXCEPTIONS=0)
    set_target_properties(aura_import_tests PROPERTIES BUILD_RPATH "$ORIGIN"
      COMPILE_DEFINITIONS "${AURA_IMPORT_TEST_DEFINITIONS}")
    if(WIN32)
      add_dependencies(aura_import_tests aura_browser)
    endif()
    gtest_discover_tests(aura_import_tests DISCOVERY_MODE PRE_TEST)
    add_executable(aura_update_manager_tests tests/update_manager_tests.cpp core/browser/updates.cpp)
    target_include_directories(aura_update_manager_tests PRIVATE "${CEF_ROOT}")
    target_compile_definitions(aura_update_manager_tests PRIVATE ${CEF_COMPILER_DEFINES})
    target_link_libraries(aura_update_manager_tests PRIVATE aura_common libcef_lib libcef_dll_wrapper
      ${AURA_BROWSER_LIBS} GTest::gtest_main)
    get_target_property(AURA_UPDATE_TEST_DEFINITIONS aura_update_manager_tests COMPILE_DEFINITIONS)
    list(REMOVE_ITEM AURA_UPDATE_TEST_DEFINITIONS _HAS_EXCEPTIONS=0)
    set_target_properties(aura_update_manager_tests PROPERTIES BUILD_RPATH "$ORIGIN"
      COMPILE_DEFINITIONS "${AURA_UPDATE_TEST_DEFINITIONS}")
    if(WIN32)
      add_dependencies(aura_update_manager_tests aura_browser)
    endif()
    gtest_discover_tests(aura_update_manager_tests DISCOVERY_MODE PRE_TEST)
    add_executable(aura_protection_tests tests/protection_tests.cpp tests/filter_lists_tests.cpp core/browser/protection.cpp core/browser/filter_lists.cpp core/browser/extensions.cpp)
    target_include_directories(aura_protection_tests PRIVATE "${CEF_ROOT}")
    target_compile_definitions(aura_protection_tests PRIVATE ${CEF_COMPILER_DEFINES} AURA_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(aura_protection_tests PRIVATE aura_common aura_adblock libcef_lib libcef_dll_wrapper OpenSSL::Crypto ${AURA_BROWSER_LIBS} GTest::gtest_main)
    get_target_property(AURA_TEST_DEFINITIONS aura_protection_tests COMPILE_DEFINITIONS)
    list(REMOVE_ITEM AURA_TEST_DEFINITIONS _HAS_EXCEPTIONS=0)
    set_target_properties(aura_protection_tests PROPERTIES BUILD_RPATH "$ORIGIN" COMPILE_DEFINITIONS "${AURA_TEST_DEFINITIONS}")
    if(WIN32)
      # Complete CEF runtime staging before any test executable can inspect it.
      add_dependencies(aura_protection_tests aura_browser)
    endif()
    gtest_discover_tests(aura_protection_tests DISCOVERY_MODE PRE_TEST)
  endif()
  if(TARGET aura_diagnostic)
    add_dependencies(aura_browser aura_diagnostic)
  endif()
  add_custom_command(TARGET aura_browser POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CEF_ROOT}/Release" "$<TARGET_FILE_DIR:aura_browser>"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CEF_ROOT}/Resources" "$<TARGET_FILE_DIR:aura_browser>"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/resources/fonts" "$<TARGET_FILE_DIR:aura_browser>/fonts"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/resources/default.toml" "$<TARGET_FILE_DIR:aura_browser>/default.toml"
    VERBATIM)
  if(WIN32)
    add_custom_command(TARGET aura_browser POST_BUILD
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/stage_windows.py"
        "$<TARGET_FILE_DIR:aura_browser>" "${CEF_ROOT}"
        "${PROJECT_SOURCE_DIR}/third_party/skia-build/windows64" "${CMAKE_MT}" aura_browser VERBATIM)
  endif()
  add_custom_target(aura_protection_resources ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/resources/protection" "$<TARGET_FILE_DIR:aura_browser>/protection"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/resources/reader" "$<TARGET_FILE_DIR:aura_browser>/reader"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/resources/vendor" "$<TARGET_FILE_DIR:aura_browser>/vendor" VERBATIM)
  add_dependencies(aura_browser aura_protection_resources)
endif()
