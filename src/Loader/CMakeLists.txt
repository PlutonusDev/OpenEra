add_library(OpenEraLoader STATIC
    PeReader.h
    PeReader.cpp
    XvdReader.h
    XvdReader.cpp
    ImageMemory.h
    ImageMemory.cpp
    ImageMapper.h
    ImageMapper.cpp
    ImportResolver.h
    ImportResolver.cpp
    HostModules.h
    HostModules.cpp
    ModuleManager.h
    ModuleManager.cpp
    LoaderShims.cpp
    UnwindRegistration.h
    UnwindRegistration.cpp)
add_library(OpenEra::Loader ALIAS OpenEraLoader)

# Core is public: it provides the shared include root ("Loader/PeReader.h").
target_link_libraries(OpenEraLoader
    PUBLIC
        OpenEra::Core
        OpenEra::Kernel
    PRIVATE
        OpenEra::CompileOptions
        OpenEra::CoreCheck)
