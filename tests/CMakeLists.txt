add_executable(test_protocol test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE dwncore)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_shell_quote test_shell_quote.cpp)
target_link_libraries(test_shell_quote PRIVATE dwncore)
add_test(NAME shell_quote COMMAND test_shell_quote)

add_executable(test_resolve test_resolve.cpp)
target_link_libraries(test_resolve PRIVATE dwncore)
add_test(NAME resolve COMMAND test_resolve)

add_executable(test_fetcher test_fetcher.cpp)
target_link_libraries(test_fetcher PRIVATE dwncore)
target_compile_definitions(test_fetcher PRIVATE
  DWN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME fetcher COMMAND test_fetcher)

add_executable(test_downloader test_downloader.cpp)
target_link_libraries(test_downloader PRIVATE dwncore)
add_test(NAME downloader COMMAND test_downloader)

# Chrome launches the host with the extension origin as argv[1]. The host must
# start its message loop rather than treat that as a CLI request; with stdin at
# EOF it exits 0. Before this test existed, it printed usage and exited 2.
add_test(NAME browser_launch
         COMMAND dwn-host chrome-extension://idnnokolakgodhfhnmcjjopbghpapifd/)
set_tests_properties(browser_launch PROPERTIES
         PASS_REGULAR_EXPRESSION "host exiting")