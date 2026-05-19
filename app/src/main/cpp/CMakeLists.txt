cmake_minimum_required(VERSION 3.22.1)
project(template_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

add_library(template_native SHARED
        template_native.cpp
        arm64_guest_framework.cpp
        native_utils.cpp
)

if(NOT DEFINED TEMPLATE_VERBOSE_LOGS)
    set(TEMPLATE_VERBOSE_LOGS 0)
endif()

target_compile_definitions(template_native PRIVATE
        TEMPLATE_VERBOSE_LOGS=${TEMPLATE_VERBOSE_LOGS}
)

target_compile_options(template_native PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -ffunction-sections -fdata-sections
        -fno-rtti -fno-exceptions
)

target_link_options(template_native PRIVATE
        -Wl,--gc-sections
        -Wl,--exclude-libs,ALL
)

target_link_libraries(template_native PRIVATE
        log
        android
        dl
)
