add_library(OpenEraCore STATIC
    ByteReader.h
    ByteReader.cpp)
add_library(OpenEra::Core ALIAS OpenEraCore)

# Headers are included as "Core/ByteReader.h".
target_include_directories(OpenEraCore PUBLIC
    ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(OpenEraCore PRIVATE
    OpenEra::CompileOptions
    OpenEra::CoreCheck)
