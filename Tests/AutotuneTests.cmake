# Include this file near the end of the project's root CMakeLists.txt:
#     include(${CMAKE_CURRENT_SOURCE_DIR}/Tests/AutotuneTests.cmake)

include(CTest)
option(AUTOTUNE_BUILD_TESTS "Build DSP and real-world regression tests" ON)

if (BUILD_TESTING AND AUTOTUNE_BUILD_TESTS)
    juce_add_console_app(AutotuneCoreRegression
        PRODUCT_NAME "Autotune Core Regression")
    juce_generate_juce_header(AutotuneCoreRegression)
    target_sources(AutotuneCoreRegression PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/ModernPitchEngineTests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../Source/ModernPitchEngine.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../Source/Tempo.cpp)
    target_include_directories(AutotuneCoreRegression PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../Source)
    target_link_libraries(AutotuneCoreRegression PRIVATE
        juce::juce_audio_basics
        juce::juce_core
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    target_compile_definitions(AutotuneCoreRegression PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    add_test(NAME AutotuneCoreRegression COMMAND AutotuneCoreRegression)

    juce_add_console_app(AutotuneRealWorldRegression
        PRODUCT_NAME "Autotune Real World Regression")
    juce_generate_juce_header(AutotuneRealWorldRegression)
    target_sources(AutotuneRealWorldRegression PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/RealWorldRegression.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../Source/ModernPitchEngine.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../Source/Tempo.cpp)
    target_include_directories(AutotuneRealWorldRegression PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../Source)
    target_link_libraries(AutotuneRealWorldRegression PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_core
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    target_compile_definitions(AutotuneRealWorldRegression PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
endif()
