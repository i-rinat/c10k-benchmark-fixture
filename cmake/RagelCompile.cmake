function(ragel_compile src dest)

    find_program(RAGEL ragel)
    if (${RAGEL} STREQUAL "RAGEL-NOTFOUND")
        message(FATAL_ERROR "Ragel not found (install /usr/bin/ragel)")
    endif()

    set(dst_full "${CMAKE_CURRENT_BINARY_DIR}/ragelcompiled_${src}")
    set(src_full "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
    add_custom_command(
        OUTPUT "${dst_full}"
        DEPENDS "${src_full}"
        COMMAND "${RAGEL}"
        ARGS -G2 "${src_full}" -o "${dst_full}"
    )

    set(${dest} "${dst_full}" PARENT_SCOPE)
endfunction()

function(create_header_symlink src)

    set(dst_full "${CMAKE_CURRENT_BINARY_DIR}/${src}")
    set(src_full "${CMAKE_CURRENT_SOURCE_DIR}/${src}")

    message(STATUS "symlink ${dst_full}")
    execute_process(COMMAND ln -s "${src_full}" "${dst_full}" ERROR_QUIET)

endfunction()
