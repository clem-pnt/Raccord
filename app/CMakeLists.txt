add_executable(raccord main.cpp)
target_link_libraries(raccord PRIVATE raccord_ui)
install(TARGETS raccord RUNTIME DESTINATION bin)
