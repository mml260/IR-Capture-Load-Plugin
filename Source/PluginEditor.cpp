#include "PluginEditor.h"

void IRCaptureLoadAudioProcessorEditor::WaveView::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff101318));
    g.setColour(juce::Colour(0xff2c323b));
    g.drawRect(getLocalBounds());
    if (data.empty()) return;
    const auto r = getLocalBounds().reduced(3);
    const float mid = r.getCentreY();
    g.setColour(accent.withAlpha(0.85f));
    juce::Path p;
    p.startNewSubPath(static_cast<float>(r.getX()), mid);
    for (size_t i = 0; i < data.size(); ++i)
    {
        const float x = r.getX() + static_cast<float>(i) * r.getWidth() / static_cast<float>(data.size() - 1);
        const float h = juce::jlimit(0.0f, 1.0f, data[i]) * r.getHeight() * 0.47f;
        p.startNewSubPath(x, mid - h);
        p.lineTo(x, mid + h);
    }
    g.strokePath(p, juce::PathStrokeType(1.2f));
}
void IRCaptureLoadAudioProcessorEditor::Meter::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff101318));
    auto r = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(juce::Colour(0xff303741)); g.drawRoundedRectangle(r, 3.0f, 1.0f);
    r = r.reduced(2.0f);
    r.setWidth(r.getWidth() * juce::jlimit(0.0f, 1.0f, (value + 60.0f) / 60.0f));
    g.setColour(value > -1.0f ? juce::Colours::red : juce::Colour(0xff54c7b5));
    g.fillRoundedRectangle(r, 2.0f);
    g.setColour(juce::Colours::white);
    g.setFont(11.0f);
    g.drawText(juce::String(value, 1) + " dBFS", getLocalBounds().reduced(5, 0), juce::Justification::centredRight);
}

IRCaptureLoadAudioProcessorEditor::IRCaptureLoadAudioProcessorEditor(IRCaptureLoadAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    setSize(800, 680);
    title.setText("IR Capture + Load", juce::dontSendNotification); title.setFont(juce::Font(20.0f, juce::Font::bold));
    title.setColour(juce::Label::textColourId, juce::Colours::white); addAndMakeVisible(title);
    for (auto* tab : { &loaderTab, &captureTab })
    { tab->setClickingTogglesState(false); addAndMakeVisible(*tab); }
    loaderTab.onClick = [this] { showPage(false); };
    captureTab.onClick = [this] { showPage(true); };

    for (int i = 0; i < 2; ++i)
    {
        auto& s = slots[static_cast<size_t>(i)];
        s.accent = i == 0 ? juce::Colour(0xff54c7d0) : juce::Colour(0xffe9ad58);
        s.slotTitle.setText(i == 0 ? "Slot A" : "Slot B", juce::dontSendNotification);
        s.slotTitle.setColour(juce::Label::textColourId, s.accent);
        s.slotTitle.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        s.load.setButtonText("Load WAV"); s.name.setText("Empty slot", juce::dontSendNotification);
        s.name.setColour(juce::Label::textColourId, juce::Colours::white);
        s.wave.setAccent(s.accent);
        s.gain.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag); s.gain.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 18);
        s.gain.setRange(-60.0, 6.0, 0.1); s.gain.setValue(processor.getSlotGainDb(i)); s.gain.setTextValueSuffix(" dB");
        s.gain.onValueChange = [this, i] { processor.setSlotGainDb(i, static_cast<float>(slots[static_cast<size_t>(i)].gain.getValue())); };
        s.enabled.setButtonText("Enabled"); s.enabled.setToggleState(processor.isSlotEnabled(i), juce::dontSendNotification);
        s.enabled.onClick = [this, i] { processor.setSlotEnabled(i, slots[static_cast<size_t>(i)].enabled.getToggleState()); };
        s.load.onClick = [this, i] { loadSlot(i); };
        slotPanels[static_cast<size_t>(i)].setName(i == 0 ? "Slot A" : "Slot B");
        loaderPage.addAndMakeVisible(slotPanels[static_cast<size_t>(i)]);
        slotPanels[static_cast<size_t>(i)].addAndMakeVisible(s.slotTitle);
        slotPanels[static_cast<size_t>(i)].addAndMakeVisible(s.load);
        slotPanels[static_cast<size_t>(i)].addAndMakeVisible(s.name);
        slotPanels[static_cast<size_t>(i)].addAndMakeVisible(s.wave);
        slotPanels[static_cast<size_t>(i)].addAndMakeVisible(s.gain);
        slotPanels[static_cast<size_t>(i)].addAndMakeVisible(s.enabled);
    }
    loaderPage.setVisible(true); capturePage.setVisible(false);
    addAndMakeVisible(loaderPage); addAndMakeVisible(capturePage);

    for (auto* label : { &inputGainLabel, &outputGainLabel })
    {
        label->setColour(juce::Label::textColourId, juce::Colours::white);
        label->setJustificationType(juce::Justification::centred);
        label->setFont(12.0f);
        addAndMakeVisible(*label);
    }
    inputGainLabel.setText("Input", juce::dontSendNotification);
    outputGainLabel.setText("Output", juce::dontSendNotification);
    for (auto* slider : { &inputGain, &outputGain })
    {
        slider->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 58, 17);
        slider->setRange(-24.0, 24.0, 0.1);
        slider->setTextValueSuffix(" dB");
        addAndMakeVisible(*slider);
    }
    inputGain.setValue(processor.getInputGainDb());
    outputGain.setValue(processor.getOutputGainDb());
    inputGain.onValueChange = [this] { processor.setInputGainDb(static_cast<float>(inputGain.getValue())); };
    outputGain.onValueChange = [this] { processor.setOutputGainDb(static_cast<float>(outputGain.getValue())); };
    for (auto* label : { &loaderStatus, &captureStatus })
    { label->setColour(juce::Label::textColourId, juce::Colour(0xffc5cbd4)); label->setFont(14.0f); }
    loaderPage.addAndMakeVisible(loaderStatus);

    inputLabel.setText("Input dBFS", juce::dontSendNotification); outputLabel.setText("Output dBFS", juce::dontSendNotification);
    for (auto* l : { &inputLabel, &outputLabel }) { l->setColour(juce::Label::textColourId, juce::Colours::white); l->setFont(12.0f); addAndMakeVisible(*l); }
    addAndMakeVisible(inputMeter); addAndMakeVisible(outputMeter);

    settings.setText("48,000 Hz  •  Log sweep 20 Hz–20 kHz  •  262,144 samples (5.46 s)  •  one repetition\n250 ms lead-in  •  1 s tail  •  200 ms IR  •  mono 32-bit float WAV", juce::dontSendNotification);
    settings.setColour(juce::Label::textColourId, juce::Colour(0xffc5cbd4)); settings.setJustificationType(juce::Justification::topLeft);
    capturePage.addAndMakeVisible(settings);
    sweepLevel.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag); sweepLevel.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 68, 20);
    sweepLevel.setRange(-60, -6, 0.1); sweepLevel.setValue(processor.getSweepLevelDb()); sweepLevel.setTextValueSuffix(" dBFS peak");
    sweepLevel.onValueChange = [this] { processor.setSweepLevelDb(static_cast<float>(sweepLevel.getValue())); };
    capturePage.addAndMakeVisible(sweepLevel);
    armButton.onClick = [this] { if (!processor.armCapture()) captureStatus.setText(processor.getCaptureStatus(), juce::dontSendNotification); };
    actionButton.onClick = [this] { if (processor.getCaptureState() == IRCaptureLoadAudioProcessor::CaptureState::Armed) processor.startCapture(); else processor.abortCapture(); };
    for (auto* b : { &armButton, &actionButton, &useAButton, &useBButton, &saveButton, &enableLoaderButton }) capturePage.addAndMakeVisible(*b);
    useAButton.onClick = [this] { processor.useResultInSlot(0); };
    useBButton.onClick = [this] { processor.useResultInSlot(1); };
    saveButton.onClick = [this] { saveCapture(); };
    enableLoaderButton.onClick = [this]
    {
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::WarningIcon, "Return to Loader",
            "First change this Logic track input to your guitar/DI source and its output to the normal monitor bus. Enable Loader now?",
            "Enable Loader", "Stay Silent", this, juce::ModalCallbackFunction::create([this](int result)
            { if (result == 1) processor.enableLoader(); }));
    };
    resultWave.setAccent(juce::Colour(0xff9f80df)); capturePage.addAndMakeVisible(resultWave);
    progress.setSliderStyle(juce::Slider::LinearBar); progress.setRange(0, 1, 0.001); progress.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0); progress.setEnabled(false);
    capturePage.addAndMakeVisible(progress);
    routing.setText(processor.getRoutingNotice(), juce::dontSendNotification); routing.setColour(juce::Label::textColourId, juce::Colour(0xffffc56e));
    routing.setJustificationType(juce::Justification::topLeft); routing.setFont(12.0f); capturePage.addAndMakeVisible(routing);
    capturePage.addAndMakeVisible(captureStatus);
    showPage(false);
    startTimerHz(25);
}
void IRCaptureLoadAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff171a20));
    g.setColour(juce::Colour(0xff252a32));
    g.fillRoundedRectangle(loaderPage.getBounds().toFloat(), 8.0f);
    g.fillRoundedRectangle(capturePage.getBounds().toFloat(), 8.0f);
    for (int i = 0; i < 2; ++i)
    {
        auto r = slotPanels[static_cast<size_t>(i)].getBounds().toFloat().translated(loaderPage.getX(), loaderPage.getY());
        g.setColour(juce::Colour(0xff21262e)); g.fillRoundedRectangle(r, 8.0f);
        g.setColour(slots[static_cast<size_t>(i)].accent); g.fillRoundedRectangle(r.withWidth(4.0f), 2.0f);
    }
}
void IRCaptureLoadAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced(20);
    auto header = r.removeFromTop(68);
    title.setBounds(header.removeFromLeft(230));
    captureTab.setBounds(header.removeFromRight(100).withY(header.getY() + 14).withHeight(38));
    loaderTab.setBounds(header.removeFromRight(92).withY(header.getY() + 14).withHeight(38));
    auto knobArea = header.removeFromRight(176);
    auto inputBounds = knobArea.removeFromLeft(88);
    auto outputBounds = knobArea;
    inputGainLabel.setBounds(inputBounds.removeFromTop(14));
    outputGainLabel.setBounds(outputBounds.removeFromTop(14));
    inputGain.setBounds(inputBounds);
    outputGain.setBounds(outputBounds);
    auto meters = r.removeFromBottom(42);
    inputLabel.setBounds(meters.removeFromLeft(88)); inputMeter.setBounds(meters.removeFromLeft(190).reduced(0, 10));
    meters.removeFromLeft(20); outputLabel.setBounds(meters.removeFromLeft(95)); outputMeter.setBounds(meters.removeFromLeft(190).reduced(0, 10));
    auto body = r.reduced(0, 8); loaderPage.setBounds(body); capturePage.setBounds(body);
    auto lr = loaderPage.getLocalBounds().reduced(12);
    loaderStatus.setBounds(lr.removeFromBottom(26)); lr.removeFromBottom(4);
    const int ph = lr.getHeight() / 2;
    slotPanels[0].setBounds(lr.removeFromTop(ph).reduced(0, 4)); slotPanels[1].setBounds(lr.reduced(0, 4));
    for (int i = 0; i < 2; ++i)
    {
        auto& s = slots[static_cast<size_t>(i)]; auto q = slotPanels[static_cast<size_t>(i)].getLocalBounds().reduced(12);
        auto top = q.removeFromTop(30);
        s.slotTitle.setBounds(top.removeFromLeft(84));
        s.load.setBounds(top.removeFromLeft(108));
        s.name.setBounds(q.removeFromTop(25));
        s.wave.setBounds(q.removeFromTop(60));
        auto bottom = q.removeFromTop(90); s.enabled.setBounds(bottom.removeFromLeft(100).withY(bottom.getY() + 20).withHeight(28));
        s.gain.setBounds(bottom.removeFromLeft(120));
    }
    auto cr = capturePage.getLocalBounds().reduced(16);
    settings.setBounds(cr.removeFromTop(54));
    auto controls = cr.removeFromTop(116); sweepLevel.setBounds(controls.removeFromLeft(120));
    auto buttons = controls.reduced(6, 28); armButton.setBounds(buttons.removeFromLeft(112)); actionButton.setBounds(buttons.removeFromLeft(108));
    progress.setBounds(cr.removeFromTop(22)); captureStatus.setBounds(cr.removeFromTop(36));
    resultWave.setBounds(cr.removeFromTop(120));
    auto resultButtons = cr.removeFromTop(38); useAButton.setBounds(resultButtons.removeFromLeft(100)); useBButton.setBounds(resultButtons.removeFromLeft(100)); saveButton.setBounds(resultButtons.removeFromLeft(110));
    enableLoaderButton.setBounds(cr.removeFromTop(36).removeFromLeft(150));
    routing.setBounds(cr.removeFromTop(56));
}
void IRCaptureLoadAudioProcessorEditor::showPage(bool capture)
{
    capturePageVisible = capture; loaderPage.setVisible(!capture); capturePage.setVisible(capture);
    loaderTab.setToggleState(!capture, juce::dontSendNotification); captureTab.setToggleState(capture, juce::dontSendNotification);
    repaint();
}
void IRCaptureLoadAudioProcessorEditor::loadSlot(int slot)
{
    chooser = std::make_unique<juce::FileChooser>("Load mono WAV impulse response", juce::File{}, "*.wav");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, slot](const juce::FileChooser& fc)
        {
            const auto file = fc.getResult(); if (file == juce::File{}) return;
            juce::String error;
            if (!processor.loadIR(slot, file, error)) loaderStatus.setText(error, juce::dontSendNotification);
            else loaderStatus.setText(processor.getSlotName(slot), juce::dontSendNotification);
            timerCallback();
        });
}
void IRCaptureLoadAudioProcessorEditor::saveCapture()
{
    chooser = std::make_unique<juce::FileChooser>("Save captured IR", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Captured IR.wav"), "*.wav");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& fc)
        {
            auto file = fc.getResult(); if (file == juce::File{}) return;
            if (file.getFileExtension().isEmpty()) file = file.withFileExtension(".wav");
            juce::String error; if (!processor.saveResultWav(file, error)) captureStatus.setText(error, juce::dontSendNotification);
        });
}
void IRCaptureLoadAudioProcessorEditor::timerCallback()
{
    inputMeter.setValue(processor.getInputPeakDb()); outputMeter.setValue(processor.getOutputPeakDb());
    const auto state = processor.getCaptureState();
    const bool capture = capturePageVisible;
    if (capture)
    {
        inputLabel.setText("Return dBFS", juce::dontSendNotification);
        outputLabel.setText("Sweep Send dBFS", juce::dontSendNotification);
        captureStatus.setText(processor.getCaptureStatus(), juce::dontSendNotification);
        progress.setValue(processor.getCaptureProgress(), juce::dontSendNotification);
        actionButton.setButtonText(state == IRCaptureLoadAudioProcessor::CaptureState::Armed ? "Start" : "Abort");
        actionButton.setEnabled(state == IRCaptureLoadAudioProcessor::CaptureState::Armed || state == IRCaptureLoadAudioProcessor::CaptureState::Recording);
        outputGain.setEnabled(false);
        armButton.setEnabled(std::abs(processor.getHostSampleRate() - 48000.0) < 0.5 &&
            (state == IRCaptureLoadAudioProcessor::CaptureState::Loader || state == IRCaptureLoadAudioProcessor::CaptureState::Error || state == IRCaptureLoadAudioProcessor::CaptureState::Ready));
        const bool ready = state == IRCaptureLoadAudioProcessor::CaptureState::Ready;
        useAButton.setEnabled(ready); useBButton.setEnabled(ready); saveButton.setEnabled(ready);
        resultWave.setData(processor.getResultWaveform());
    }
    else
    {
        outputGain.setEnabled(true);
        inputLabel.setText("Input dBFS", juce::dontSendNotification);
        outputLabel.setText("Output dBFS", juce::dontSendNotification);
        const bool any = (!processor.getSlotName(0).isEmpty() && processor.isSlotEnabled(0)) || (!processor.getSlotName(1).isEmpty() && processor.isSlotEnabled(1));
        loaderStatus.setText(any ? "Both slots receive the same mono input and sum to mono." : "No enabled IRs: output is silent.", juce::dontSendNotification);
    }
    for (int i = 0; i < 2; ++i)
    {
        const auto name = processor.getSlotName(i);
        slots[static_cast<size_t>(i)].name.setText(name.isEmpty() ? "Empty slot" : name, juce::dontSendNotification);
        slots[static_cast<size_t>(i)].wave.setData(processor.getSlotWaveform(i));
    }
    const bool rateOk = std::abs(processor.getHostSampleRate() - 48000.0) < 0.5;
    armButton.setTooltip(rateOk ? "Arm the silent capture path." : "Set the Logic project sample rate to 48 kHz first.");
    inputGain.setTooltip("Input level for Loader playback and the captured mic return.");
    outputGain.setTooltip("Loader output level. Sweep Level controls the capture send.");
}
