# tests/CMakeLists.txt — wired only when -DBUILD_TESTING=ON.

# Example placeholder; replace with Qt Test based tests as they're added.
add_executable(frame_parser_test frame_parser_test.cpp)
add_test(NAME frame_parser_test COMMAND frame_parser_test)
