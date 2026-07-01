add_executable(fft_tests test_fft.cpp)
target_link_libraries(fft_tests PRIVATE dsp_dispatch)
add_test(NAME fft_tests COMMAND fft_tests)
