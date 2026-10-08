# Reads the status_led field of a board.yml: which LED the board has and how it is driven.
#   scrivo_board_led(<board.yml> <out_platform> <out_pin> <out_active>)
# platform is none when the field is absent, neopixel for an addressable LED, gpio for a plain one; pin is the GPIO
# number; active is the level that lights a gpio LED, 0 or 1, and empty for the others. The field, in the words our
# engine reads (module_core/pin/gpio.py) and names its LED module with (module_ext/led-status-neopixel):
#   status_led:
#     platform: neopixel | gpio
#     pin: GPIO<n>
#     inverted: true | false      (gpio only, may be left out: true lights the LED on a low level, else on high)
# Anything else under the field stops the build with the file named.
#
# As a script it prints what it read: cmake -D BOARD_YML=<board.yml> -P cmake/board_led.cmake

function(scrivo_board_led board_yml out_platform out_pin out_active)
    file(READ ${board_yml} text)
    # Comments and trailing blanks go first, so a comment line inside the field does not end it.
    string(REGEX REPLACE "#[^\n]*" "" text "\n${text}\n")
    string(REGEX REPLACE "[ \t]+\n" "\n" text "${text}")
    string(REGEX REPLACE "\n\n+" "\n" text "${text}")

    set(platform none)
    set(pin "")
    set(active "")
    string(REGEX MATCH "\nstatus_led:[^\n]*\n([ \t][^\n]*\n)*" field "${text}")
    if(field)
        string(REGEX MATCH "^\nstatus_led:([^\n]*)\n" head "${field}")
        if(NOT CMAKE_MATCH_1 STREQUAL "")
            message(FATAL_ERROR "${board_yml}: status_led takes its values on the lines below it, not \"${CMAKE_MATCH_1}\"")
        endif()
        string(REGEX REPLACE "^\nstatus_led:\n" "" body "${field}")
        string(REGEX MATCHALL "[^\n]+" lines "${body}")
        foreach(line IN LISTS lines)
            if(NOT line MATCHES "^[ \t]+([a-z_]+):[ \t]*([^ \t]*)$")
                message(FATAL_ERROR "${board_yml}: status_led has a line it cannot read: \"${line}\"")
            endif()
            # taken now: the next MATCHES overwrites CMAKE_MATCH_n
            set(key ${CMAKE_MATCH_1})
            set(value "${CMAKE_MATCH_2}")
            if(NOT key MATCHES "^(platform|pin|inverted)$")
                message(FATAL_ERROR "${board_yml}: status_led has no key ${key}; it takes platform, pin and inverted")
            endif()
            set(field_${key} "${value}")
        endforeach()

        if(NOT DEFINED field_platform)
            message(FATAL_ERROR "${board_yml}: status_led has no platform: neopixel or gpio")
        endif()
        if(NOT field_platform MATCHES "^(neopixel|gpio)$")
            message(FATAL_ERROR "${board_yml}: status_led platform ${field_platform} is neither neopixel nor gpio")
        endif()
        if(NOT DEFINED field_pin)
            message(FATAL_ERROR "${board_yml}: status_led has no pin")
        endif()
        if(NOT field_pin MATCHES "^GPIO([0-9]+)$")
            message(FATAL_ERROR "${board_yml}: status_led pin ${field_pin} is not of the form GPIO<n>")
        endif()
        set(pin ${CMAKE_MATCH_1})

        if(field_platform STREQUAL "neopixel")
            if(DEFINED field_inverted)
                message(FATAL_ERROR "${board_yml}: status_led inverted is for a gpio LED; a neopixel takes its colour, not a level")
            endif()
        else()
            # Our engine drives a gpio pin without inverted as a plain Pin, lit on a high level.
            set(active 1)
            if(DEFINED field_inverted)
                string(TOLOWER "${field_inverted}" inverted)
                if(inverted STREQUAL "true")
                    set(active 0)
                elseif(NOT inverted STREQUAL "false")
                    message(FATAL_ERROR "${board_yml}: status_led inverted ${field_inverted} is neither true nor false")
                endif()
            endif()
        endif()
        set(platform ${field_platform})
    endif()

    set(${out_platform} ${platform} PARENT_SCOPE)
    set(${out_pin} "${pin}" PARENT_SCOPE)
    set(${out_active} "${active}" PARENT_SCOPE)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    scrivo_board_led(${BOARD_YML} platform pin active)
    message("status_led platform=${platform} pin=${pin} active=${active}")
endif()
