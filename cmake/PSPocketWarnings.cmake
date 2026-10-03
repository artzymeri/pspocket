# pspocket_set_warnings(<target>): the project's warning set. Applied to first-party targets only.
function(pspocket_set_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wnon-virtual-dtor -Wcast-align -Wunused -Woverloaded-virtual
        -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
        $<$<COMPILE_LANGUAGE:C>:-Wno-unused-function>
        # Generated and decoder code relies on these; keep them quiet.
        -Wno-gnu-zero-variadic-macro-arguments
        -Wno-gnu-anonymous-struct -Wno-nested-anon-types -Wno-c99-extensions)
    if(PSPOCKET_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()

# Generated (lifted) code gets a reduced set: it is machine-written and intentionally repetitive.
function(pspocket_set_generated_warnings target)
    target_compile_options(${target} PRIVATE -Wno-unused-variable -Wno-unused-but-set-variable
        -Wno-unused-label -Wno-unused-function -Wno-unused-parameter)
endfunction()
