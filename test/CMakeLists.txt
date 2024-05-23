add_executable(peglib_test main.cpp)
target_include_directories(peglib_test PUBLIC ../include)
add_test(NAME peglib_test COMMAND peglib_test)

add_executable(peglib_test_lua lua_lex.cpp)
find_package(Boost REQUIRED COMPONENTS unit_test_framework)
target_include_directories(peglib_test_lua PUBLIC ../include)
target_link_libraries(peglib_test_lua PUBLIC Boost::unit_test_framework)
add_test(NAME peglib_test_lua COMMAND peglib_test_lua)
