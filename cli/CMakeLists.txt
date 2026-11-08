add_executable(raccord-cli main.cpp)
target_link_libraries(raccord-cli PRIVATE raccord_engine)
install(TARGETS raccord-cli RUNTIME DESTINATION bin)
