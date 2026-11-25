add_library(OpenEraLoader STATIC
    PeReader.h
    PeReader.cpp
    XvdReader.h
    XvdReader.cpp)
add_library(OpenEra::Loader ALIAS OpenEraLoader)

# Core is public: it provides the shared include root ("Loader/PeReader.h").
target_link_libraries(OpenEraLoader
    PUBLIC
        OpenEra::Core
    PRIVATE
        OpenEra::CompileOptions
        OpenEra::CoreCheck)
