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
    LoggerTests.cpp
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

catch_discover_tests(OpenEraTests)
