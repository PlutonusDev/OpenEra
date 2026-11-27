# Test images mapped by OpenEra, not the OS loader. They all land in one
# directory, which the tests read with DirectoryModuleReader.
set(OPENERA_FIXTURE_DIR ${CMAKE_CURRENT_BINARY_DIR}/images PARENT_SCOPE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/images)

# Runtime checks need the C runtime's start-up, which EntryImage doesn't have.
string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")

# The DLLs use the release DLL C runtime, which host passthrough allows.
add_library(OpenEraEhImage SHARED EhImage.cpp)
add_library(OpenEraStartImage SHARED StartImage.cpp)
add_library(OpenEraSyncImage SHARED SyncImage.cpp)
add_library(OpenEraCrtFileImage SHARED CrtFileImage.cpp)
set_target_properties(OpenEraEhImage OpenEraStartImage OpenEraSyncImage OpenEraCrtFileImage PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set_target_properties(OpenEraEhImage PROPERTIES OUTPUT_NAME EhImage)
set_target_properties(OpenEraStartImage PROPERTIES OUTPUT_NAME StartImage)
set_target_properties(OpenEraSyncImage PROPERTIES OUTPUT_NAME SyncImage)
set_target_properties(OpenEraCrtFileImage PROPERTIES OUTPUT_NAME CrtFileImage)
# It calls fopen and friends on purpose.
target_compile_definitions(OpenEraCrtFileImage PRIVATE _CRT_SECURE_NO_WARNINGS)

add_executable(OpenEraEntryImage EntryImage.cpp)
set_target_properties(OpenEraEntryImage PROPERTIES OUTPUT_NAME EntryImage)
target_compile_options(OpenEraEntryImage PRIVATE /GS-)
target_link_options(OpenEraEntryImage PRIVATE /NODEFAULTLIB /ENTRY:EntryImageStart /SUBSYSTEM:CONSOLE)

# Checks it sees itself as the process image; also without the C runtime.
add_executable(OpenEraIdentityImage IdentityImage.cpp)
set_target_properties(OpenEraIdentityImage PROPERTIES OUTPUT_NAME IdentityImage)
target_compile_options(OpenEraIdentityImage PRIVATE /GS-)
target_link_options(OpenEraIdentityImage PRIVATE /NODEFAULTLIB /ENTRY:IdentityImageStart /SUBSYSTEM:CONSOLE)
target_link_libraries(OpenEraIdentityImage PRIVATE kernel32)

# Ends the process in different ways, without the C runtime.
add_executable(OpenEraExitImage ExitImage.cpp)
set_target_properties(OpenEraExitImage PROPERTIES OUTPUT_NAME ExitImage)
target_compile_options(OpenEraExitImage PRIVATE /GS-)
target_link_options(OpenEraExitImage PRIVATE /NODEFAULTLIB /ENTRY:ExitImageStart /SUBSYSTEM:CONSOLE)
target_link_libraries(OpenEraExitImage PRIVATE kernel32)

# A plain /MD program whose main returns, so the C runtime calls exit.
add_executable(OpenEraCrtExitImage CrtExitImage.cpp)
set_target_properties(OpenEraCrtExitImage PROPERTIES OUTPUT_NAME CrtExitImage MSVC_RUNTIME_LIBRARY MultiThreadedDLL)

add_custom_target(OpenEraFixtures DEPENDS OpenEraEhImage OpenEraStartImage OpenEraSyncImage OpenEraCrtFileImage OpenEraEntryImage
    OpenEraIdentityImage OpenEraExitImage OpenEraCrtExitImage)
