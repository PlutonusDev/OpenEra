add_library(OpenEraApp STATIC
    LogWindow.h
    LogWindow.cpp
    TitleLauncher.h
    TitleLauncher.cpp)
add_library(OpenEra::App ALIAS OpenEraApp)

target_link_libraries(OpenEraApp
    PUBLIC
        OpenEra::Core
        OpenEra::Loader
        OpenEra::Kernel
    PRIVATE
        OpenEra::CompileOptions
        OpenEra::CoreCheck)

# The openera command-line launcher. OpenEraTlsSlot.dll must sit next to it.
add_executable(openera Main.cpp)
target_link_libraries(openera PRIVATE
    OpenEra::App
    OpenEra::CompileOptions
    OpenEra::CoreCheck)
add_custom_command(TARGET openera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:OpenEraTlsSlot> $<TARGET_FILE_DIR:openera>)
