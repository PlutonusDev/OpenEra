add_library(OpenEraCore STATIC
    ByteReader.h
    ByteReader.cpp
    Logger.h
    Logger.cpp
    Xml.h
    Xml.cpp
    Assert.h
    Assert.cpp
    Config.h
    Config.cpp
    DebuggerSink.cpp)
add_library(OpenEra::Core ALIAS OpenEraCore)

# Headers are included as "Core/ByteReader.h".
target_include_directories(OpenEraCore PUBLIC
    ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(OpenEraCore PRIVATE
    OpenEra::CompileOptions
    OpenEra::CoreCheck)
