#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

class IRCaptureLoadAudioProcessor final : public juce::AudioProcessor
{
public:
    enum class CaptureState { Loader, Armed, Recording, Processing, Ready, Error };
    IRCaptureLoadAudioProcessor();
    ~IRCaptureLoadAudioProcessor() override;

    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "IR Capture + Load"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.25; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    bool loadIR(int slot, const juce::File& file, juce::String& error);
    void setSlotEnabled(int slot, bool enabled);
    bool isSlotEnabled(int slot) const;
    void setSlotGainDb(int slot, float db);
    float getSlotGainDb(int slot) const;
    void setInputGainDb(float db) { inputGainDb.store(juce::jlimit(-24.0f, 24.0f, db)); }
    float getInputGainDb() const { return inputGainDb.load(); }
    void setOutputGainDb(float db) { outputGainDb.store(juce::jlimit(-24.0f, 24.0f, db)); }
    float getOutputGainDb() const { return outputGainDb.load(); }
    juce::String getSlotName(int slot) const;
    std::vector<float> getSlotWaveform(int slot) const;
    std::vector<float> getSlotStartSamples(int slot, int count = 480) const;
    bool saveSlotWav(int slot, const juce::File& file, juce::String& error) const;
    void setSlotBOffsetMs(float ms);
    float getSlotBOffsetMs() const { return slotBOffsetMs.load(); }
    float autoAlignSlotB();

    void setSweepLevelDb(float db) { sweepLevelDb.store(juce::jlimit(-60.0f, -6.0f, db)); }
    float getSweepLevelDb() const { return sweepLevelDb.load(); }
    bool armCapture();
    void startCapture();
    void abortCapture(const juce::String& reason = "Capture aborted");
    void enableLoader() { captureState.store(CaptureState::Loader); }
    CaptureState getCaptureState() const { return captureState.load(); }
    juce::String getCaptureStatus() const;
    float getCaptureProgress() const;
    juce::String getRoutingNotice() const;
    std::vector<float> getResultWaveform() const;
    std::vector<float> getResultSpectrum() const;
    bool useResultInSlot(int slot);
    bool saveResultWav(const juce::File& file, juce::String& error) const;
    double getHostSampleRate() const { return hostSampleRate.load(); }
    float getInputPeakDb() const { return inputPeakDb.load(); }
    float getOutputPeakDb() const { return outputPeakDb.load(); }
    bool isCaptureClipping() const { return captureClipping.load(); }

private:
    struct Slot
    {
        juce::dsp::Convolution convolution;
        juce::AudioBuffer<float> ir;
        juce::String name;
        double sampleRate = 48000.0;
        std::atomic<float> gainDb { -12.0f };
        std::atomic<bool> enabled { true };
        std::atomic<bool> loaded { false };
        std::atomic<bool> truncated { false };
        juce::SmoothedValue<float> gain;
    };
    class CaptureWorker final : public juce::Thread
    {
    public:
        explicit CaptureWorker(IRCaptureLoadAudioProcessor& p) : Thread("IR capture extraction"), owner(p) {}
        void run() override;
    private:
        IRCaptureLoadAudioProcessor& owner;
    };

    void extractCapture();
    bool setIR(int slot, const float* samples, int count, double rate, const juce::String& name, juce::String& error);
    static bool decodeAndExtract(const std::vector<float>& recorded, const std::vector<float>& inverseSweep,
                                 int leadSamples, int sampleRate, std::vector<float>& result, juce::String& error);
    static juce::String stateName(CaptureState);
    static constexpr int captureRate = 48000;
    static constexpr int sweepSamples = 262144;
    static constexpr int leadSamples = 12000;
    static constexpr int tailSamples = 48000;
    static constexpr int maxIrSamples = 9600;

    Slot slots[2];
    juce::AudioFormatManager formats;
    juce::AudioBuffer<float> workA, workB;
    juce::SmoothedValue<float> inputGain, outputGain;
    juce::SmoothedValue<float> slotBDelaySamples;
    juce::CriticalSection dataLock;
    std::vector<float> emittedSweep, inverseSweep, recordedReturn, resultIR;
    juce::WaitableEvent extractionReady;
    CaptureWorker captureWorker;
    std::atomic<CaptureState> captureState { CaptureState::Loader };
    std::atomic<double> hostSampleRate { 0.0 };
    std::atomic<float> sweepLevelDb { -20.0f }, inputPeakDb { -100.0f }, outputPeakDb { -100.0f };
    std::atomic<float> slotBOffsetMs { 0.0f };
    std::atomic<float> inputGainDb { 0.0f }, outputGainDb { 0.0f };
    std::atomic<int> capturePosition { 0 };
    std::atomic<bool> captureClipping { false };
    std::atomic<bool> captureBlockOverflow { false };
    std::atomic<bool> cancelExtraction { false };
    std::atomic<bool> extractionBusy { false };
    std::array<float, 2048> slotADelay {}, slotBDelay {};
    int delayPosition = 0;
    juce::String statusText { "Load one or two mono WAV impulse responses." };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IRCaptureLoadAudioProcessor)
};
