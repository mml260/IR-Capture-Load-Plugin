#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <array>

class IRCaptureLoadAudioProcessorEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit IRCaptureLoadAudioProcessorEditor(IRCaptureLoadAudioProcessor&);
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    class WaveView final : public juce::Component
    {
    public:
        void setData(std::vector<float> d) { data = std::move(d); repaint(); }
        void setAccent(juce::Colour c) { accent = c; }
        void paint(juce::Graphics&) override;
    private:
        std::vector<float> data;
        juce::Colour accent { 0xff55c8cc };
    };
    class Meter final : public juce::Component
    {
    public:
        void setValue(float db) { value = juce::jlimit(-60.0f, 0.0f, db); repaint(); }
        void paint(juce::Graphics&) override;
    private:
        float value = -100.0f;
    };
    struct SlotControls
    {
        juce::Label slotTitle;
        juce::TextButton load;
        juce::Label name;
        WaveView wave;
        juce::Slider gain;
        juce::ToggleButton enabled;
        juce::Colour accent;
    };
    void timerCallback() override;
    void loadSlot(int slot);
    void saveCapture();
    void showPage(bool capture);
    IRCaptureLoadAudioProcessor& processor;
    juce::TextButton loaderTab { "Loader" }, captureTab { "Capture" };
    juce::Label title, inputGainLabel, outputGainLabel, loaderStatus, captureStatus, routing, settings, inputLabel, outputLabel;
    std::array<SlotControls, 2> slots;
    std::array<juce::TextButton, 2> useButtons { juce::TextButton("Use in A"), juce::TextButton("Use in B") };
    std::array<juce::Component, 2> slotPanels;
    juce::Component loaderPage, capturePage;
    WaveView resultWave;
    Meter inputMeter, outputMeter;
    juce::Slider inputGain, outputGain, sweepLevel, progress;
    juce::TextButton armButton { "Arm Capture" }, actionButton { "Start" }, useAButton { "Use in A" }, useBButton { "Use in B" }, saveButton { "Save WAV" }, enableLoaderButton { "Enable Loader" };
    std::unique_ptr<juce::FileChooser> chooser;
    bool capturePageVisible = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IRCaptureLoadAudioProcessorEditor)
};
