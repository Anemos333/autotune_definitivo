#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include <iostream>
#include <chrono>

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    if (argc < 2)
    {
        std::cerr << "Usage: RealWorldTests <wav_file>" << std::endl;
        return 1;
    }

    juce::File inputFile(juce::String::fromUTF8(argv[1]));
    if (!inputFile.existsAsFile())
    {
        std::cerr << "File not found: " << inputFile.getFullPathName().toStdString() << std::endl;
        return 1;
    }

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(inputFile));
    if (reader == nullptr)
    {
        std::cerr << "Could not read audio file." << std::endl;
        return 1;
    }

    juce::AudioBuffer<float> originalBuffer(reader->numChannels, static_cast<int>(reader->lengthInSamples));
    reader->read(&originalBuffer, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);

    std::cout << "Loaded file: " << inputFile.getFileName().toStdString() << " (" << reader->sampleRate << " Hz, " << reader->lengthInSamples << " samples)" << std::endl;

    for (int mode = 0; mode < 4; ++mode)
    {
        std::cout << "\n--- Testing Mode " << mode << " ---" << std::endl;

        MicrotonalAutotuneAudioProcessor processor;
        processor.updateProcessingMode(mode);
        processor.prepareToPlay(reader->sampleRate, 512);

        juce::AudioBuffer<float> processBuffer;
        processBuffer.makeCopyOf(originalBuffer);
        juce::MidiBuffer midiBuffer;

        int numSamples = processBuffer.getNumSamples();
        int blockSize = 512;
        int totalBlocks = (numSamples + blockSize - 1) / blockSize;

        auto start_time = std::chrono::high_resolution_clock::now();

        int discontinuityCount = 0;
        int zeroCount = 0;
        int maxConsecutiveZeros = 0;
        float previousSample = 0.0f;

        for (int i = 0; i < totalBlocks; ++i)
        {
            int startSample = i * blockSize;
            int numThisBlock = std::min(blockSize, numSamples - startSample);
            
            juce::AudioBuffer<float> blockBuffer(processBuffer.getArrayOfWritePointers(), processBuffer.getNumChannels(), startSample, numThisBlock);
            processor.processBlock(blockBuffer, midiBuffer);

            // Analysis for wind effect (discontinuities or random interruptions)
            const float* channelData = blockBuffer.getReadPointer(0);
            for (int s = 0; s < numThisBlock; ++s) {
                float sample = channelData[s];
                
                // Discontinuity: sudden jump
                if (std::abs(sample - previousSample) > 0.3f) {
                    discontinuityCount++;
                }

                // Interruption: exact zero or extremely low value
                if (std::abs(sample) < 1e-6f) {
                    zeroCount++;
                } else {
                    if (zeroCount > maxConsecutiveZeros) maxConsecutiveZeros = zeroCount;
                    zeroCount = 0;
                }
                
                previousSample = sample;
            }
        }
        if (zeroCount > maxConsecutiveZeros) maxConsecutiveZeros = zeroCount;

        auto end_time = std::chrono::high_resolution_clock::now();
        double time_taken_sec = std::chrono::duration<double>(end_time - start_time).count();
        double audio_length_sec = numSamples / reader->sampleRate;
        double cpu_load = (time_taken_sec / audio_length_sec) * 100.0;

        std::cout << "Latency: " << processor.getLatencySamples() << " samples (" << (processor.getLatencySamples() * 1000.0 / reader->sampleRate) << " ms)" << std::endl;
        std::cout << "Processing time: " << time_taken_sec << " s (CPU Load: " << cpu_load << " %)" << std::endl;

        if (discontinuityCount > 10 || maxConsecutiveZeros > 50) {
            std::cout << "Test Effetto Vento: Probabilmente non superato (Discontinuità: " << discontinuityCount << ", Max zeri cons: " << maxConsecutiveZeros << ")" << std::endl;
        } else {
            std::cout << "Test Effetto Vento: Nessuna discontinuità grave rilevata." << std::endl;
        }

        juce::File outputFile = inputFile.getSiblingFile(inputFile.getFileNameWithoutExtension() + "_Mode" + juce::String(mode) + ".wav");
        std::unique_ptr<juce::AudioFormatWriter> writer(formatManager.findFormatForFileExtension("wav")->createWriterFor(new juce::FileOutputStream(outputFile), reader->sampleRate, processBuffer.getNumChannels(), 16, {}, 0));
        if (writer != nullptr)
        {
            writer->writeFromAudioSampleBuffer(processBuffer, 0, processBuffer.getNumSamples());
            std::cout << "Saved: " << outputFile.getFileName().toStdString() << std::endl;
        }
    }

    return 0;
}
