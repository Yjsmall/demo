include(FetchContent)

FetchContent_Declare(
    context_reader_miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 77d0dce8627735138c51770d1799a1ef48f2117d
    GIT_SHALLOW FALSE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(context_reader_miniz)

if(TARGET miniz AND NOT TARGET Miniz::Miniz)
    add_library(Miniz::Miniz ALIAS miniz)
endif()
