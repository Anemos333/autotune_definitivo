cmake_minimum_required(VERSION 3.22)
project(MicrotonalAutotune VERSION 1.0.0 LANGUAGES C CXX OBJCXX)


set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- SCARICAMENTO AUTOMATICO DI JUCE ---
include(FetchContent)
FetchContent_Declare(
    juce
    GIT_REPOSITORY "https://github.com/juce-framework/JUCE.git"
    GIT_TAG        8.0.12 # Scarica l'ultima versione stabile di JUCE 8
)
FetchContent_MakeAvailable(juce)


# ----------------------------------------

# Se hai Antigravity come cartella locale o repo, assicurati che il percorso non sia fisso (C:/...)
# Se è una cartella dentro al tuo progetto, usa ${CMAKE_CURRENT_SOURCE_DIR}/nome_cartella


# Binary data for background images
juce_add_binary_data(BinaryData
    NAMESPACE BinaryData
    SOURCES
        Resources/sfondo1.jpeg
        Resources/sfondo2.jpg
        Resources/sfondo3.jpeg
)

# Plugin target — VST3 only
juce_add_plugin(MicrotonalAutotune
    COMPANY_NAME "AnemosMusic"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    PLUGIN_MANUFACTURER_CODE Mtal
    PLUGIN_CODE Matc
    FORMATS VST3 AU
    PRODUCT_NAME "Microtonal Autotune"
)

juce_generate_juce_header(MicrotonalAutotune)

target_sources(MicrotonalAutotune
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/ScaleDefinitions.cpp
        Source/CustomScalePresets.cpp
        Source/CustomScaleEditor.cpp
        Source/ModernPitchEngine.cpp
        Source/Tempo.cpp
)

target_compile_definitions(MicrotonalAutotune
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(MicrotonalAutotune
    PRIVATE
        BinaryData
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_plugin_client
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
