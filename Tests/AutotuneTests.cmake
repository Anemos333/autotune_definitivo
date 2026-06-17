# Include this file near the end of the project's root CMakeLists.txt:
#     include(${CMAKE_CURRENT_SOURCE_DIR}/Tests/AutotuneTests.cmake)

include(CTest)
option(AUTOTUNE_BUILD_TESTS "Build DSP and real-world regression tests" ON)
option(AUTOTUNE_ENABLE_LONG_TESTS "Register the full long-running stress profile" OFF)

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

    juce_add_console_app(AutotuneProcessorStress
        PRODUCT_NAME "Autotune Processor Stress")
    juce_generate_juce_header(AutotuneProcessorStress)
    target_sources(AutotuneProcessorStress PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/ProcessorStressTests.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../Source/PluginProcessor.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../Source/ScaleDefinitions.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../Source/CustomScalePresets.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../Source/ModernPitchEngine.cpp
        ${CMAKE_CURRENT_LIST_DIR}/../Source/Tempo.cpp)
    target_include_directories(AutotuneProcessorStress PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../Source)
    target_link_libraries(AutotuneProcessorStress PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_core
        juce::juce_data_structures
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    target_compile_definitions(AutotuneProcessorStress PRIVATE
        AUTOTUNE_HEADLESS_TEST=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)
    add_test(NAME AutotuneProcessorStress COMMAND AutotuneProcessorStress)
    set_tests_properties(AutotuneProcessorStress PROPERTIES TIMEOUT 900)
    if (AUTOTUNE_ENABLE_LONG_TESTS)
        add_test(NAME AutotuneProcessorStressLong
                 COMMAND AutotuneProcessorStress --long)
        set_tests_properties(AutotuneProcessorStressLong PROPERTIES TIMEOUT 14400)
    endif()

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
