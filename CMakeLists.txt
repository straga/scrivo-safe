# scrivo-safe: recovery image for the factory partition, built for one board per build directory:
#   idf.py -B build-<BOARD> -D SCRIVO_BOARD=<BOARD> build
cmake_minimum_required(VERSION 3.16)

# The board comes from its own folder in boards/ here, named as MicroPython names it: what the hardware gives
# (board.yml, the status LED is read from it), the bootloader options and partition table every image on the board
# shares, what this image needs of its own on that board, and the image version, whose prefix is the board.
if(NOT SCRIVO_BOARD)
    message(FATAL_ERROR "No board named: idf.py -B build-<BOARD> -D SCRIVO_BOARD=<BOARD> build, <BOARD> a folder in boards/")
endif()
set(scrivo_board_dir ${CMAKE_CURRENT_LIST_DIR}/boards/${SCRIVO_BOARD})
if(NOT EXISTS ${scrivo_board_dir}/board.yml OR NOT EXISTS ${scrivo_board_dir}/bootloader.sdkconfig OR
   NOT EXISTS ${scrivo_board_dir}/version.txt)
    message(FATAL_ERROR "boards/${SCRIVO_BOARD} has no board.yml, bootloader.sdkconfig or version.txt")
endif()
# The chip is the board's. Left to itself, IDF guesses the target from a sdkconfig in the project root before
# it reads the defaults, so a file left there by a build for another board builds this one for the wrong chip.
file(STRINGS ${scrivo_board_dir}/bootloader.sdkconfig scrivo_target REGEX "^CONFIG_IDF_TARGET=")
string(REGEX REPLACE "^CONFIG_IDF_TARGET=\"?([a-z0-9]+)\"?$" "\\1" scrivo_target "${scrivo_target}")
if(NOT scrivo_target)
    message(FATAL_ERROR "boards/${SCRIVO_BOARD}/bootloader.sdkconfig sets no CONFIG_IDF_TARGET")
endif()
if(DEFINED ENV{IDF_TARGET} AND NOT "$ENV{IDF_TARGET}" STREQUAL "${scrivo_target}")
    message(FATAL_ERROR "IDF_TARGET=$ENV{IDF_TARGET} in the environment, and board ${SCRIVO_BOARD} is ${scrivo_target}")
endif()
set(IDF_TARGET ${scrivo_target})
set(SDKCONFIG_DEFAULTS sdkconfig.defaults boards/${SCRIVO_BOARD}/bootloader.sdkconfig)
if(EXISTS ${scrivo_board_dir}/sdkconfig.defaults)
    list(APPEND SDKCONFIG_DEFAULTS boards/${SCRIVO_BOARD}/sdkconfig.defaults)
endif()
set(SDKCONFIG ${CMAKE_BINARY_DIR}/sdkconfig)
file(STRINGS ${scrivo_board_dir}/version.txt PROJECT_VER LIMIT_COUNT 1)
# Read at configure time only: without this a raised version stays out of the next build, and the image goes to the
# board under the version of the one before.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${scrivo_board_dir}/version.txt)

# DNS server that answers every query with the access point address, for the captive portal.
# Taken from the IDF example as is, so it follows the IDF version instead of a local copy.
set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/protocols/http_server/captive_portal/components/dns_server)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only main and the components it requires: the image has to fit the factory partition of the board, and
# web_size below prints what it leaves there.
idf_build_set_property(MINIMAL_BUILD ON)

# The status LED of the board, from the status_led field of its board.yml (cmake/board_led.cmake). The C code gets the
# platform, the pin and the lit level as definitions and never a pin number of its own; a board without the field
# builds with no LED. How bright the LED burns is the image's choice and stays in main/status_led.c.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/board_led.cmake)
scrivo_board_led(${scrivo_board_dir}/board.yml led_platform led_pin led_active)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${scrivo_board_dir}/board.yml)
if(led_platform STREQUAL "neopixel")
    idf_build_set_property(COMPILE_DEFINITIONS "SCRIVO_LED_NEOPIXEL;SCRIVO_LED_PIN=${led_pin}" APPEND)
elseif(led_platform STREQUAL "gpio")
    idf_build_set_property(COMPILE_DEFINITIONS "SCRIVO_LED_GPIO;SCRIVO_LED_PIN=${led_pin};SCRIVO_LED_ACTIVE=${led_active}" APPEND)
endif()
message(STATUS "status LED of ${SCRIVO_BOARD}: platform ${led_platform}, pin ${led_pin}, lit at level ${led_active}")

project(scrivo-safe)

# The portal page. web/ is the source: tools/build_web.py inlines it, refuses external links,
# compresses it and prints the sizes; target_add_binary_data embeds the result, and main.c takes
# its length from the linker symbols. zopfli comes through uv; without uv the IDF python uses gzip -9.
idf_build_get_property(idf_python PYTHON)
idf_build_get_property(partition_table_bin PARTITION_TABLE_BIN_PATH)
find_program(UV_PROGRAM uv)
if(UV_PROGRAM)
    set(web_python ${UV_PROGRAM} run --no-project --with zopfli python)
else()
    set(web_python ${idf_python})
endif()
set(web_source ${CMAKE_SOURCE_DIR}/web)
set(web_page ${CMAKE_BINARY_DIR}/web/index.html.gz)
file(GLOB web_files CONFIGURE_DEPENDS ${web_source}/*)
add_custom_command(OUTPUT ${web_page} ${CMAKE_BINARY_DIR}/web/index.html
    COMMAND ${web_python} ${CMAKE_SOURCE_DIR}/tools/build_web.py build --web ${web_source} --out ${web_page}
    DEPENDS ${web_files} ${CMAKE_SOURCE_DIR}/tools/build_web.py
    COMMENT "Building the web page from web/"
    VERBATIM)
add_custom_target(web_page DEPENDS ${web_page})
target_add_binary_data(${CMAKE_PROJECT_NAME}.elf ${web_page} BINARY DEPENDS web_page)

# Sizes at every build: the page before and after compression, and what the image leaves of factory.
add_custom_target(web_size ALL
    COMMAND ${web_python} ${CMAKE_SOURCE_DIR}/tools/build_web.py report
            --source ${CMAKE_BINARY_DIR}/web/index.html --page ${web_page}
            --image ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin --table ${partition_table_bin} --partition factory
    DEPENDS app partition_table_bin
    VERBATIM)
