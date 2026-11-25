include(FetchContent)

# SYSTEM marks Catch2's headers as external, so /W4 /WX in our tests
# does not apply to them.
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE
    SYSTEM)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

add_executable(OpenEraTests
    ByteReaderTests.cpp
    ExceptionTests.cpp
    HostModulesTests.cpp
    ImageMapperTests.cpp
    ImportResolverTests.cpp
    LoggerTests.cpp
    ModuleManagerTests.cpp
    PeReaderTests.cpp
    ShimModuleTests.cpp
    StubRegistryTests.cpp
    XvdReaderTests.cpp)

target_link_libraries(OpenEraTests PRIVATE
    OpenEra::Core
    OpenEra::Loader
    OpenEra::Kernel
    OpenEra::CompileOptions
    Catch2::Catch2WithMain)

# Shared test helpers are included as "Support/...".
target_include_directories(OpenEraTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Test image mapped by OpenEra, not the OS loader. It uses the release DLL C
# runtime, which host passthrough allows.
add_library(OpenEraEhImage SHARED Fixtures/EhImage.cpp)
set_target_properties(OpenEraEhImage PROPERTIES
    OUTPUT_NAME EhImage
    MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
add_dependencies(OpenEraTests OpenEraEhImage)
target_compile_definitions(OpenEraTests PRIVATE OPENERA_EH_IMAGE_DIR="$<TARGET_FILE_DIR:OpenEraEhImage>")

catch_discover_tests(OpenEraTests)
