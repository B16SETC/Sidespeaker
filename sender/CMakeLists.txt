option(SIDESPEAKER_CONSOLE "Build the console front-end instead of the tray app" OFF)

set(CORE
    src/config.cpp
    src/net.cpp
    src/usbmuxd.cpp
    src/adb.cpp
    src/engine.cpp
)

if(WIN32 AND NOT SIDESPEAKER_CONSOLE)
    add_executable(sidespeaker WIN32 ${CORE} src/main_tray.cpp)
else()
    add_executable(sidespeaker ${CORE} src/main_console.cpp)
endif()

target_compile_features(sidespeaker PRIVATE cxx_std_17)
target_compile_definitions(sidespeaker PRIVATE NOMINMAX)

if(WIN32)
    target_sources(sidespeaker PRIVATE src/capture_wasapi.cpp)
    target_link_libraries(sidespeaker PRIVATE ws2_32 ole32 winmm user32 shell32 gdi32)
else()
    message(FATAL_ERROR "No audio capture backend for this platform yet")
endif()

set_target_properties(sidespeaker PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_custom_command(TARGET sidespeaker POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/config.ini $<TARGET_FILE_DIR:sidespeaker>)

install(TARGETS sidespeaker RUNTIME DESTINATION .)
install(FILES config.ini DESTINATION .)
