add_library(OpenEraKernel STATIC
    StubRegistry.h
    StubRegistry.cpp)
add_library(OpenEra::Kernel ALIAS OpenEraKernel)

target_link_libraries(OpenEraKernel
    PUBLIC
        OpenEra::Core
    PRIVATE
        OpenEra::CompileOptions
        OpenEra::CoreCheck)
