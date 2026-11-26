add_library(OpenEraGraphics STATIC
    AdapterSelection.h
    AdapterSelection.cpp
    DxgiFactory.h
    DxgiFactory.cpp
    GraphicsShims.h
    GraphicsShims.cpp)
add_library(OpenEra::Graphics ALIAS OpenEraGraphics)

target_link_libraries(OpenEraGraphics
    PUBLIC
        OpenEra::Core
        OpenEra::Kernel
    PRIVATE
        OpenEra::CompileOptions
        OpenEra::CoreCheck
        dxgi
        d3d12)
