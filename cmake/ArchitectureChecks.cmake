function(context_reader_assert_target_links_only target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Architecture check references missing target: ${target}")
    endif()

    set(allowed_targets)
    foreach(allowed_target IN LISTS ARGN)
        if(TARGET "${allowed_target}")
            get_target_property(canonical_allowed_target "${allowed_target}" ALIASED_TARGET)
        endif()

        if(canonical_allowed_target)
            list(APPEND allowed_targets "${canonical_allowed_target}")
        else()
            list(APPEND allowed_targets "${allowed_target}")
        endif()

        unset(canonical_allowed_target)
    endforeach()

    get_target_property(link_libraries "${target}" LINK_LIBRARIES)

    if(NOT link_libraries)
        return()
    endif()

    foreach(link_library IN LISTS link_libraries)
        if(TARGET "${link_library}")
            get_target_property(canonical_link_library "${link_library}" ALIASED_TARGET)
        endif()

        if(NOT canonical_link_library)
            set(canonical_link_library "${link_library}")
        endif()

        if(TARGET "${link_library}" AND NOT canonical_link_library IN_LIST allowed_targets)
            message(
                FATAL_ERROR
                "Target ${target} links forbidden project target ${link_library}. "
                "Allowed project targets: ${allowed_targets}"
            )
        endif()

        unset(canonical_link_library)
    endforeach()
endfunction()
