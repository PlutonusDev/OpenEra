# Test images mapped by OpenEra, not the OS loader. They all land in one
# directory, which the tests read with DirectoryModuleReader.
set(OPENERA_FIXTURE_DIR ${CMAKE_CURRENT_BINARY_DIR}/images PARENT_SCOPE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/images)

# Runtime checks need the C runtime's start-up, which EntryImage doesn't have.
string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")

# The DLLs use the release DLL C runtime, which host passthrough allows.
add_library(OpenEraEhImage SHARED EhImage.cpp)
add_library(OpenEraStartImage SHARED StartImage.cpp)
set_target_properties(OpenEraEhImage OpenEraStartImage PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set_target_properties(OpenEraEhImage PROPERTIES OUTPUT_NAME EhImage)
set_target_properties(OpenEraStartImage PROPERTIES OUTPUT_NAME StartImage)

add_executable(OpenEraEntryImage EntryImage.cpp)
set_target_properties(OpenEraEntryImage PROPERTIES OUTPUT_NAME EntryImage)
target_compile_options(OpenEraEntryImage PRIVATE /GS-)
target_link_options(OpenEraEntryImage PRIVATE /NODEFAULTLIB /ENTRY:EntryImageStart /SUBSYSTEM:CONSOLE)

add_custom_target(OpenEraFixtures DEPENDS OpenEraEhImage OpenEraStartImage OpenEraEntryImage)
