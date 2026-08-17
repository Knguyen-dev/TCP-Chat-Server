function(set_project_warnings target_name)
    set(CLANG_WARNINGS
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
        -Werror
    )

    set(GCC_WARNINGS
        ${CLANG_WARNINGS}
        -Wmisleading-indentation
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op
        -Wnull-dereference
    )



    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # NOTE: Private means these warnings are only applied to our targets.
        # Anyone linking to our targets (e.g., the libraries) won't be inherited 
        # these warnings
        target_compile_options(${target_name} PRIVATE ${CLANG_WARNINGS})
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${target_name} PRIVATE ${GCC_WARNINGS})
    endif()
endfunction()