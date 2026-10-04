# SPDX-License-Identifier: GPL-2.0-only
file(GET_RUNTIME_DEPENDENCIES
    LIBRARIES "${DRIVER}"
    DIRECTORIES ${SODIUM_DIRS}
    PRE_INCLUDE_REGEXES "^libsodium\\.so"
    PRE_EXCLUDE_REGEXES ".*"
    RESOLVED_DEPENDENCIES_VAR dependencies
    UNRESOLVED_DEPENDENCIES_VAR unresolved)
if(unresolved OR NOT dependencies)
    message(FATAL_ERROR "Cannot resolve the driver's shared libsodium dependency: ${unresolved}")
endif()
get_filename_component(destination "${DRIVER}" DIRECTORY)
foreach(dependency IN LISTS dependencies)
    file(COPY "${dependency}" FOLLOW_SYMLINK_CHAIN DESTINATION "${destination}")
endforeach()
