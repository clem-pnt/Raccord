find_package(Catch2 REQUIRED)
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS *.cpp)
add_executable(raccord-tests ${TEST_SOURCES})
target_link_libraries(raccord-tests PRIVATE raccord_engine Catch2::Catch2)
add_test(NAME raccord-tests COMMAND raccord-tests)
target_compile_definitions(raccord-tests PRIVATE RACCORD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Interface test: drives the real main window (needs a display, e.g. Xvfb).
find_package(Qt6 COMPONENTS Test)
if(Qt6Test_FOUND)
  add_executable(raccord-uitest ui/uitest.cpp)
  target_link_libraries(raccord-uitest PRIVATE raccord_ui Qt6::Test)
endif()
