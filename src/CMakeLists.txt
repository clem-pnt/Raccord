file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
  core/*.cpp core/*.h media/*.cpp media/*.h render/*.cpp render/*.h
  audio/*.cpp audio/*.h export/*.cpp export/*.h interop/*.cpp interop/*.h ai/*.cpp ai/*.h ofx/*.cpp ofx/*.h)
add_library(raccord_engine STATIC ${ENGINE_SOURCES} ${CMAKE_SOURCE_DIR}/resources/engine.qrc)
target_include_directories(raccord_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(raccord_engine PUBLIC Qt6::Core Qt6::Gui Qt6::OpenGL Qt6::Concurrent Qt6::Xml
  PkgConfig::FFMPEG Threads::Threads ZLIB::ZLIB)
if(WIN32)
  target_compile_definitions(raccord_engine PUBLIC _USE_MATH_DEFINES NOMINMAX)
endif()

file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS ui/*.cpp ui/*.h playback/*.cpp playback/*.h)
add_library(raccord_ui STATIC ${UI_SOURCES} ${CMAKE_SOURCE_DIR}/resources/ui.qrc)
target_link_libraries(raccord_ui PUBLIC raccord_engine Qt6::Widgets Qt6::OpenGLWidgets Qt6::Multimedia)
