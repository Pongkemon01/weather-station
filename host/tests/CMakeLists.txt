find_package(Qt6 6.8 REQUIRED COMPONENTS Core SerialPort Test)

# ---- frame_parser_test -------------------------------------------------------
qt_add_executable(frame_parser_test
    frame_parser_test.cpp
    ../src/frame_parser.cpp
    ../src/frame_parser.h
)

target_include_directories(frame_parser_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
)

target_link_libraries(frame_parser_test PRIVATE
    Qt6::Core
    Qt6::Test
)

add_test(NAME frame_parser_test COMMAND frame_parser_test)

# ---- q97_conversion_test -----------------------------------------------------
qt_add_executable(q97_conversion_test
    q97_conversion_test.cpp
)

target_include_directories(q97_conversion_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
)

target_link_libraries(q97_conversion_test PRIVATE
    Qt6::Core
    Qt6::Test
)

add_test(NAME q97_conversion_test COMMAND q97_conversion_test)

# ---- device_controller_test --------------------------------------------------
qt_add_executable(device_controller_test
    device_controller_test.cpp
    ../src/device_controller.cpp
    ../src/device_controller.h
    ../src/device_io.cpp
    ../src/device_io.h
    ../src/frame_parser.cpp
    ../src/frame_parser.h
    ../src/log_buffer.cpp
    ../src/log_buffer.h
)

target_include_directories(device_controller_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
)

target_link_libraries(device_controller_test PRIVATE
    Qt6::Core
    Qt6::SerialPort
    Qt6::Test
)

add_test(NAME device_controller_test COMMAND device_controller_test)
