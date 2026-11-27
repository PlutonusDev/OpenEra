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
    AdapterSelectionTests.cpp
    AssertTests.cpp
    ByteReaderTests.cpp
    ConfigTests.cpp
    ExceptionTests.cpp
    FileMemoryShimTests.cpp
    GameConfigTests.cpp
    GraphicsShimTests.cpp
    HostModulesTests.cpp
    ImageInitializerTests.cpp
    ImageMapperTests.cpp
    ImportResolverTests.cpp
    LogWindowTests.cpp
    LoggerTests.cpp
    ModuleManagerTests.cpp
    PeReaderTests.cpp
    ProcessImageTests.cpp
    ShimModuleTests.cpp
    StubRegistryTests.cpp
    ThreadShimTests.cpp
    TimeSyncTests.cpp
    TitleFileSystemTests.cpp
    TitleLauncherTests.cpp
    XmlTests.cpp
    XvdReaderTests.cpp)

target_link_libraries(OpenEraTests PRIVATE
    OpenEra::App
    OpenEra::Core
    OpenEra::Graphics
    OpenEra::Loader
    OpenEra::Kernel
    OpenEra::CompileOptions
    Catch2::Catch2WithMain)

# Shared test helpers are included as "Support/...".
target_include_directories(OpenEraTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(Fixtures)

# Fails assertions on request, for AssertTests.
add_executable(OpenEraAssertHelper Helpers/AssertHelper.cpp)
set_target_properties(OpenEraAssertHelper PROPERTIES OUTPUT_NAME AssertHelper)
target_link_libraries(OpenEraAssertHelper PRIVATE OpenEra::Core OpenEra::CompileOptions)

add_dependencies(OpenEraTests OpenEraFixtures OpenEraTlsSlot OpenEraAssertHelper)
target_compile_definitions(OpenEraTests PRIVATE
    OPENERA_FIXTURE_DIR="${OPENERA_FIXTURE_DIR}"
    OPENERA_TLS_SLOT="$<TARGET_FILE:OpenEraTlsSlot>"
    OPENERA_ASSERT_HELPER=L"$<TARGET_FILE:OpenEraAssertHelper>")

catch_discover_tests(OpenEraTests)
