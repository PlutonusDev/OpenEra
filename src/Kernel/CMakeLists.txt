add_library(OpenEraKernel STATIC
    ShimGuard.h
    ShimGuard.cpp
    ShimModule.h
    ShimModule.cpp
    StubRegistry.h
    StubRegistry.cpp
    TitleFileSystem.h
    TitleFileSystem.cpp
    FileShims.h
    FileShims.cpp
    MemoryShims.h
    MemoryShims.cpp)
add_library(OpenEra::Kernel ALIAS OpenEraKernel)

target_link_libraries(OpenEraKernel
    PUBLIC
        OpenEra::Core
    PRIVATE
        OpenEra::CompileOptions
        OpenEra::CoreCheck)
