set(HASHTABLE_SRCS hashtable.c hashtable_itr.c)
set(BELLADONNA_SRCS unionfs.cpp opts.c debug.c findbranch.c readdir.c
    general.c unlink.c cow.c cow_utils.c string.c rmdir.c usyslog.c
    fuse_ops.c diffgen.cpp)

SET(_COMMON_FLAGS "-pipe -W -Wall -D_FORTIFY_SOURCE=2 -D_FILE_OFFSET_BITS=64")
SET(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g ${_COMMON_FLAGS}")
SET(CMAKE_C_FLAGS_RELEASE "-O2 ${_COMMON_FLAGS}")
SET(CMAKE_CXX_FLAGS_RELEASE "-O2 ${_COMMON_FLAGS}")
SET(CMAKE_C_FLAGS_DEBUG "-O0 -g -DDEBUG ${_COMMON_FLAGS} -fsanitize=address")
SET(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -DDEBUG ${_COMMON_FLAGS} -fsanitize=address")

add_library(belladonna ${BELLADONNA_SRCS} ${HASHTABLE_SRCS})

option(WITH_LIBFUSE3 "Enable libfuse3 support" ON)

add_definitions(-DFUSE_USE_VERSION=317)

target_link_libraries(belladonna ${FUSE_LIBRARIES} archive_static)
target_include_directories(belladonna PUBLIC include/)

add_custom_command(TARGET belladonna POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_BINARY_DIR}/compile_commands.json
        ${CMAKE_SOURCE_DIR}/compile_commands.json
    COMMENT "Add clangd to the src directory")
