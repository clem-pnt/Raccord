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
  target_compile_definitions(raccord-uitest PRIVATE RACCORD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  target_link_libraries(raccord-uitest PRIVATE raccord_ui Qt6::Test)
endif()

# OpenFX test plugin, laid out as a standard bundle in the build tree.
set(RACCORD_OFX_TEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/ofx)
add_library(raccord-ofx-test MODULE ofx/TestPlugin.cpp)
target_include_directories(raccord-ofx-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
set_target_properties(raccord-ofx-test PROPERTIES PREFIX "" OUTPUT_NAME "RaccordTest" SUFFIX ".ofx"
  CXX_VISIBILITY_PRESET hidden
  LIBRARY_OUTPUT_DIRECTORY ${RACCORD_OFX_TEST_DIR}/RaccordTest.ofx.bundle/Contents/Linux-x86-64)
add_dependencies(raccord-tests raccord-ofx-test)
target_compile_definitions(raccord-tests PRIVATE RACCORD_OFX_TEST_DIR="${RACCORD_OFX_TEST_DIR}")
