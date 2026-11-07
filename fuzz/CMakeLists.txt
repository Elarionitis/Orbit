# libFuzzer targets. Configure with clang and -DORBIT_BUILD_FUZZERS=ON.
function(orbit_add_fuzzer name)
    add_executable(${name} ${name}.cpp)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined -g -O1)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(${name} PRIVATE ${ARGN})
endfunction()

orbit_add_fuzzer(fuzz_protocol orbit_protocol)
orbit_add_fuzzer(fuzz_wal orbit_storage)
