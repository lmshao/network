add_executable(unix_echo_server unix_echo_server.cxx)
target_link_libraries(unix_echo_server network)

add_executable(unix_client unix_client.cxx)
target_link_libraries(unix_client network)
