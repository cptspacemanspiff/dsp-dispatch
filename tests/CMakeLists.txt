add_executable(fft_tests test_fft.cpp)
target_link_libraries(fft_tests PRIVATE dsp_dispatch)
add_test(NAME fft_tests COMMAND fft_tests)

add_executable(fir_tests test_fir.cpp)
target_link_libraries(fir_tests PRIVATE dsp_dispatch)
add_test(NAME fir_tests COMMAND fir_tests)
