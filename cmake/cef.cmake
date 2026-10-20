if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Aura M0 requires an x64 toolchain")
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  message(FATAL_ERROR "The locked minimal CEF SDK contains Release libraries only. Use a Release preset.")
endif()
if(WIN32)
  set(AURA_PLATFORM windows64)
  set(USE_SANDBOX ON CACHE BOOL "" FORCE)
  # Aura uses Win32/WRL directly. Avoid the SDK ATL search walking outside VS.
  set(USE_ATL OFF CACHE BOOL "Aura does not require ATL" FORCE)
  # llvm-mt does not support updating bootstrap.exe resources in place.
  find_program(AURA_WINDOWS_MT mt.exe HINTS "$ENV{WindowsSdkVerBinPath}/x64" REQUIRED)
  set(CMAKE_MT "${AURA_WINDOWS_MT}")
else()
  set(AURA_PLATFORM linux64)
endif()
set(CEF_ROOT "${PROJECT_SOURCE_DIR}/third_party/src/cef-${AURA_PLATFORM}")
if(NOT EXISTS "${CEF_ROOT}/cmake/FindCEF.cmake")
  message(FATAL_ERROR "Missing CEF. Run python scripts/bootstrap.py")
endif()
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  # Ninja already controls parallelism; clang-cl rejects MSVC's /MP under /WX.
  list(REMOVE_ITEM CEF_COMPILER_FLAGS /MP)
endif()
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" "libcef_dll_wrapper")
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  # GetProcAddress returns FARPROC; the SDK casts it to the exported signature.
  # Sized C API aggregates intentionally zero-initialize their remaining fields.
  target_compile_options(libcef_dll_wrapper PRIVATE
    -Wno-error=cast-function-type-mismatch -Wno-missing-field-initializers
    # The SDK already applies this to Clang on Linux for cross-TU specializations.
    -Wno-undefined-var-template)
endif()
ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
