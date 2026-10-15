#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <utility>

namespace
{
constexpr float silenceDb = -100.0f;
float toDb(float peak) { return peak > 1.0e-6f ? juce::Decibels::gainToDecibels(peak) : silenceDb; }
}

IRCaptureLoadAudioProcessor::IRCaptureLoadAudioProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::mono(), true)
                                       .withOutput("Output", juce::AudioChannelSet::mono(), true)),
      captureWorker(*this)
{
    formats.registerBasicFormats();
    captureWorker.startThread();
}
juce::AudioProcessorEditor* IRCaptureLoadAudioProcessor::createEditor()
{
    return new IRCaptureLoadAudioProcessorEditor(*this);
}

IRCaptureLoadAudioProcessor::~IRCaptureLoadAudioProcessor()
{
    captureWorker.signalThreadShouldExit();
    extractionReady.signal();
    captureWorker.stopThread(-1);
}

void IRCaptureLoadAudioProcessor::prepareToPlay(double sampleRate, int maxBlock)
{
    if (captureState.load() != CaptureState::Loader)
    {
        cancelExtraction.store(true);
        captureState.store(CaptureState::Error, std::memory_order_release);
        const juce::ScopedLock lock(dataLock);
        statusText = "Host reset or sample-rate change aborted capture. Previous slot IRs are preserved.";
    }
    hostSampleRate.store(sampleRate);
    const auto scratch = juce::jmax(65536, maxBlock);
    workA.setSize(1, scratch, false, false, true);
    workB.setSize(1, scratch, false, false, true);
    inputGain.reset(sampleRate, 0.03);
    outputGain.reset(sampleRate, 0.03);
    slotBDelaySamples.reset(sampleRate, 0.01);
    slotADelay.fill(0.0f);
    slotBDelay.fill(0.0f);
    delayPosition = 0;
    inputGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(inputGainDb.load()));
    outputGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(outputGainDb.load()));
    slotBDelaySamples.setCurrentAndTargetValue(static_cast<float>(juce::roundToInt(sampleRate * 0.004)));
    const juce::dsp::ProcessSpec spec { sampleRate, static_cast<juce::uint32>(scratch), 1 };
    for (auto& slot : slots)
    {
        slot.convolution.prepare(spec);
        slot.gain.reset(sampleRate, 0.03);
        slot.gain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(slot.gainDb.load()));
        if (slot.loaded.load())
        {
            juce::AudioBuffer<float> copy(1, slot.ir.getNumSamples());
            copy.copyFrom(0, 0, slot.ir, 0, 0, slot.ir.getNumSamples());
            slot.convolution.loadImpulseResponse(std::move(copy), slot.sampleRate,
                juce::dsp::Convolution::Stereo::no, juce::dsp::Convolution::Trim::no,
                juce::dsp::Convolution::Normalise::no);
        }
    }
    setLatencySamples(juce::jmax(slots[0].convolution.getLatency(), slots[1].convolution.getLatency())
                      + juce::roundToInt(sampleRate * 0.004));
}
void IRCaptureLoadAudioProcessor::releaseResources() {}
bool IRCaptureLoadAudioProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    return l.getMainInputChannelSet() == juce::AudioChannelSet::mono()
        && l.getMainOutputChannelSet() == juce::AudioChannelSet::mono();
}

void IRCaptureLoadAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    midi.clear();
    if (buffer.getNumChannels() == 0) return;
    const int n = buffer.getNumSamples();
    if (n > workA.getNumSamples())
    {
        if (captureState.load() != CaptureState::Loader)
        {
            captureBlockOverflow.store(true);
            cancelExtraction.store(true);
            captureState.store(CaptureState::Error, std::memory_order_release);
        }
        buffer.clear();
        return;
    }

    auto* out = buffer.getWritePointer(0);
    const auto* in = buffer.getReadPointer(0);
    float inPeak = 0.0f;
    for (int i = 0; i < n; ++i)
        if (std::isfinite(in[i])) inPeak = juce::jmax(inPeak, std::abs(in[i]));
    inputPeakDb.store(toDb(inPeak));

    auto state = captureState.load(std::memory_order_acquire);
    if (state != CaptureState::Loader)
    {
        inputGain.setTargetValue(juce::Decibels::decibelsToGain(inputGainDb.load()));
        float outPeak = 0.0f;
        float recordedPeak = 0.0f;
        const int length = static_cast<int>(recordedReturn.size());
        int pos = capturePosition.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            float sample = 0.0f;
            if (state == CaptureState::Recording && pos < length)
            {
                    const float rawReturn = in[i];
                    const float ret = rawReturn * inputGain.getNextValue();
                    recordedPeak = juce::jmax(recordedPeak, std::abs(ret));
                    if (!std::isfinite(rawReturn) || !std::isfinite(ret) || std::abs(rawReturn) >= 0.999f || std::abs(ret) >= 0.999f)
                {
                    captureClipping.store(true);
                    captureState.store(CaptureState::Error, std::memory_order_release);
                    state = CaptureState::Error;
                    capturePosition.store(pos);
                }
                else
                {
                    recordedReturn[static_cast<size_t>(pos)] = ret;
                    const int sweepIndex = pos - leadSamples;
                    if (sweepIndex >= 0 && sweepIndex < static_cast<int>(emittedSweep.size()))
                        sample = emittedSweep[static_cast<size_t>(sweepIndex)];
                    ++pos;
                    capturePosition.store(pos, std::memory_order_relaxed);
                    if (pos >= length)
                    {
                        extractionBusy.store(true, std::memory_order_release);
                        captureState.store(CaptureState::Processing, std::memory_order_release);
                        extractionReady.signal();
                        state = CaptureState::Processing;
                    }
                }
            }
            out[i] = sample;
            outPeak = juce::jmax(outPeak, std::abs(sample));
        }
        if (state == CaptureState::Recording || captureState.load() == CaptureState::Recording)
            inputPeakDb.store(toDb(recordedPeak));
        outputPeakDb.store(toDb(outPeak));
        return;
    }

    workA.copyFrom(0, 0, buffer, 0, 0, n);
    const float inputTarget = juce::Decibels::decibelsToGain(inputGainDb.load());
    inputGain.setTargetValue(inputTarget);
    auto* inputProcessed = workA.getWritePointer(0);
    for (int i = 0; i < n; ++i) inputProcessed[i] *= inputGain.getNextValue();
    workB.copyFrom(0, 0, workA, 0, 0, n);
    juce::FloatVectorOperations::clear(out, n);
    juce::dsp::AudioBlock<float> blockA(workA.getArrayOfWritePointers(), 1, 0, static_cast<size_t>(n));
    juce::dsp::AudioBlock<float> blockB(workB.getArrayOfWritePointers(), 1, 0, static_cast<size_t>(n));
    float finalPeak = 0.0f;
    for (int s = 0; s < 2; ++s)
    {
        auto& slot = slots[s];
        if (!slot.loaded.load(std::memory_order_acquire) || !slot.enabled.load())
        {
            (s == 0 ? workA : workB).clear(0, n);
            continue;
        }
        auto& work = s == 0 ? workA : workB;
        auto& block = s == 0 ? blockA : blockB;
        slot.convolution.process(juce::dsp::ProcessContextReplacing<float>(block));
        auto* source = work.getWritePointer(0);
        const float targetGain = juce::Decibels::decibelsToGain(slot.gainDb.load());
        slot.gain.setTargetValue(targetGain);
        for (int i = 0; i < n; ++i)
        {
            source[i] *= slot.gain.getNextValue();
        }
    }
    const int baseDelay = juce::roundToInt(hostSampleRate.load() * 0.004);
    const int offsetDelay = juce::roundToInt(hostSampleRate.load() * slotBOffsetMs.load() * 0.001f);
    slotBDelaySamples.setTargetValue(static_cast<float>(juce::jlimit(0, 2 * baseDelay, baseDelay + offsetDelay)));
    auto* sourceA = workA.getReadPointer(0);
    auto* sourceB = workB.getReadPointer(0);
    for (int i = 0; i < n; ++i)
    {
        slotADelay[static_cast<size_t>(delayPosition)] = sourceA[i];
        slotBDelay[static_cast<size_t>(delayPosition)] = sourceB[i];
        const int delayA = baseDelay;
        const float delayB = slotBDelaySamples.getNextValue();
        const int readA = (delayPosition - delayA + static_cast<int>(slotADelay.size())) % static_cast<int>(slotADelay.size());
        const int delayB0 = static_cast<int>(delayB);
        const int delayB1 = juce::jmin(delayB0 + 1, static_cast<int>(slotBDelay.size()) - 1);
        const int readB0 = (delayPosition - delayB0 + static_cast<int>(slotBDelay.size())) % static_cast<int>(slotBDelay.size());
        const int readB1 = (delayPosition - delayB1 + static_cast<int>(slotBDelay.size())) % static_cast<int>(slotBDelay.size());
        const float a = delayA == 0 ? sourceA[i] : slotADelay[static_cast<size_t>(readA)];
        const float delayBFrac = delayB - delayB0;
        const float b = delayB <= 0.0f ? sourceB[i]
            : slotBDelay[static_cast<size_t>(readB0)] * (1.0f - delayBFrac)
              + slotBDelay[static_cast<size_t>(readB1)] * delayBFrac;
        out[i] = a + b;
        finalPeak = juce::jmax(finalPeak, std::abs(out[i]));
        delayPosition = (delayPosition + 1) % static_cast<int>(slotADelay.size());
    }
    const float outputTarget = juce::Decibels::decibelsToGain(outputGainDb.load());
    outputGain.setTargetValue(outputTarget);
    finalPeak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        out[i] *= outputGain.getNextValue();
        finalPeak = juce::jmax(finalPeak, std::abs(out[i]));
    }
    outputPeakDb.store(toDb(finalPeak));
}

bool IRCaptureLoadAudioProcessor::loadIR(int slot, const juce::File& file, juce::String& error)
{
    if (slot < 0 || slot > 1) { error = "Choose slot A or B."; return false; }
    if (file.getFileExtension().toLowerCase() != ".wav") { error = "Choose a WAV file."; return false; }
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (!reader) { error = "Choose a readable WAV file."; return false; }
    if (reader->numChannels != 1) { error = "IR files must be mono WAVs."; return false; }
    if (reader->sampleRate < 8000.0 || reader->sampleRate > 192000.0) { error = "Unsupported WAV sample rate."; return false; }
    const auto maxFrames = static_cast<juce::int64>(std::ceil(reader->sampleRate * 0.2));
    const int count = static_cast<int>(juce::jmin(reader->lengthInSamples, maxFrames));
    if (count < 1) { error = "The WAV file is empty."; return false; }
    juce::AudioBuffer<float> data(1, count);
    if (!reader->read(&data, 0, count, 0, true, false)) { error = "Could not read the WAV samples."; return false; }
    const bool wasTruncated = reader->lengthInSamples > count;
    if (wasTruncated)
        for (int i = juce::jmax(0, count - static_cast<int>(reader->sampleRate * 0.01)); i < count; ++i)
            data.setSample(0, i, data.getSample(0, i) * (count - 1 - i) / static_cast<float>(juce::jmax(1, static_cast<int>(reader->sampleRate * 0.01))));
    if (!setIR(slot, data.getReadPointer(0), count, reader->sampleRate, file.getFileName(), error)) return false;
    slots[slot].truncated.store(wasTruncated);
    return true;
}

bool IRCaptureLoadAudioProcessor::setIR(int slot, const float* samples, int count, double rate,
                                        const juce::String& name, juce::String& error)
{
    if (slot < 0 || slot > 1 || samples == nullptr || count <= 0) { error = "No valid impulse response to assign."; return false; }
    auto& dst = slots[slot];
    {
        const juce::ScopedLock lock(dataLock);
        dst.ir.setSize(1, count);
        dst.ir.copyFrom(0, 0, samples, count);
        dst.sampleRate = rate;
        dst.name = name;
    }
    juce::AudioBuffer<float> copy(1, count);
    copy.copyFrom(0, 0, samples, count);
    dst.convolution.loadImpulseResponse(std::move(copy), rate, juce::dsp::Convolution::Stereo::no,
        juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
    dst.loaded.store(true, std::memory_order_release);
    dst.truncated.store(false);
    error.clear();
    return true;
}
void IRCaptureLoadAudioProcessor::setSlotEnabled(int slot, bool on) { if (slot >= 0 && slot < 2) slots[slot].enabled.store(on); }
bool IRCaptureLoadAudioProcessor::isSlotEnabled(int slot) const { return slot >= 0 && slot < 2 && slots[slot].enabled.load(); }
void IRCaptureLoadAudioProcessor::setSlotGainDb(int slot, float db) { if (slot >= 0 && slot < 2) slots[slot].gainDb.store(juce::jlimit(-60.0f, 6.0f, db)); }
float IRCaptureLoadAudioProcessor::getSlotGainDb(int slot) const { return slot >= 0 && slot < 2 ? slots[slot].gainDb.load() : -12.0f; }
juce::String IRCaptureLoadAudioProcessor::getSlotName(int slot) const
{
    if (slot < 0 || slot > 1) return {};
    const juce::ScopedLock lock(dataLock);
    return slots[slot].name + (slots[slot].truncated.load() ? " (capped at 200 ms)" : juce::String{});
}
std::vector<float> IRCaptureLoadAudioProcessor::getSlotWaveform(int slot) const
{
    std::vector<float> v;
    if (slot < 0 || slot > 1) return v;
    const juce::ScopedLock lock(dataLock);
    const auto& b = slots[slot].ir;
    if (b.getNumSamples() == 0) return v;
    constexpr int bins = 240;
    v.resize(bins);
    for (int x = 0; x < bins; ++x)
    {
        const int a = x * b.getNumSamples() / bins;
        const int z = juce::jmax(a + 1, (x + 1) * b.getNumSamples() / bins);
        float peak = 0.0f;
        for (int i = a; i < juce::jmin(z, b.getNumSamples()); ++i) peak = juce::jmax(peak, std::abs(b.getSample(0, i)));
        v[static_cast<size_t>(x)] = peak;
    }
    return v;
}

std::vector<float> IRCaptureLoadAudioProcessor::getSlotStartSamples(int slot, int count) const
{
    if (slot < 0 || slot > 1 || count <= 0) return {};
    const juce::ScopedLock lock(dataLock);
    const auto& ir = slots[slot].ir;
    if (ir.getNumSamples() == 0) return {};
    std::vector<float> samples(static_cast<size_t>(count), 0.0f);
    const double step = slots[slot].sampleRate / 48000.0;
    const auto* source = ir.getReadPointer(0);
    for (int i = 0; i < count; ++i)
    {
        const double position = i * step;
        const int index = static_cast<int>(position);
        if (index >= ir.getNumSamples()) break;
        const int next = juce::jmin(index + 1, ir.getNumSamples() - 1);
        const float frac = static_cast<float>(position - index);
        samples[static_cast<size_t>(i)] = source[index] + (source[next] - source[index]) * frac;
    }
    return samples;
}

void IRCaptureLoadAudioProcessor::setSlotBOffsetMs(float ms)
{
    slotBOffsetMs.store(juce::jlimit(-2.0f, 2.0f, ms));
}

float IRCaptureLoadAudioProcessor::autoAlignSlotB()
{
    const auto a = getSlotStartSamples(0, 960);
    const auto b = getSlotStartSamples(1, 960);
    if (a.empty() || b.empty()) return slotBOffsetMs.load();
    constexpr int transientWindow = 480; // first 10 ms at 48 kHz
    auto peakIndex = [](const std::vector<float>& samples)
    {
        constexpr int window = 480;
        int peak = 0;
        float magnitude = 0.0f;
        for (int i = 0; i < juce::jmin(window, static_cast<int>(samples.size())); ++i)
        {
            const float candidate = std::abs(samples[static_cast<size_t>(i)]);
            if (candidate > magnitude) { magnitude = candidate; peak = i; }
        }
        return std::pair<int, float>(peak, magnitude);
    };
    const auto [peakA, levelA] = peakIndex(a);
    const auto [peakB, levelB] = peakIndex(b);
    if (levelA < 1.0e-6f || levelB < 1.0e-6f) return slotBOffsetMs.load();
    const int peakDifference = peakA - peakB;
    const int boundedDifference = juce::jlimit(-transientWindow, transientWindow, peakDifference);
    const float result = juce::jlimit(-2.0f, 2.0f, boundedDifference * 1000.0f / 48000.0f);
    setSlotBOffsetMs(result);
    return result;
}

void IRCaptureLoadAudioProcessor::swapSlotIRs()
{
    juce::AudioBuffer<float> copyA, copyB;
    double rateA = 48000.0, rateB = 48000.0;
    bool loadedA = false, loadedB = false;
    {
        const juce::ScopedLock lock(dataLock);
        juce::AudioBuffer<float> temp(std::move(slots[0].ir));
        slots[0].ir = std::move(slots[1].ir);
        slots[1].ir = std::move(temp);
        std::swap(slots[0].sampleRate, slots[1].sampleRate);
        std::swap(slots[0].name, slots[1].name);
        const bool truncatedA = slots[0].truncated.load();
        slots[0].truncated.store(slots[1].truncated.load());
        slots[1].truncated.store(truncatedA);
        loadedA = slots[1].loaded.load();
        loadedB = slots[0].loaded.load();
        slots[0].loaded.store(loadedA);
        slots[1].loaded.store(loadedB);
        rateA = slots[0].sampleRate;
        rateB = slots[1].sampleRate;
        if (loadedA && slots[0].ir.getNumSamples() > 0)
        {
            copyA.setSize(1, slots[0].ir.getNumSamples());
            copyA.copyFrom(0, 0, slots[0].ir, 0, 0, slots[0].ir.getNumSamples());
        }
        if (loadedB && slots[1].ir.getNumSamples() > 0)
        {
            copyB.setSize(1, slots[1].ir.getNumSamples());
            copyB.copyFrom(0, 0, slots[1].ir, 0, 0, slots[1].ir.getNumSamples());
        }
    }
    if (loadedA && copyA.getNumSamples() > 0)
        slots[0].convolution.loadImpulseResponse(std::move(copyA), rateA, juce::dsp::Convolution::Stereo::no,
            juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
    if (loadedB && copyB.getNumSamples() > 0)
        slots[1].convolution.loadImpulseResponse(std::move(copyB), rateB, juce::dsp::Convolution::Stereo::no,
            juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
    setSlotBOffsetMs(0.0f);
}

bool IRCaptureLoadAudioProcessor::armCapture()
{
    if (extractionBusy.load(std::memory_order_acquire))
    {
        const juce::ScopedLock lock(dataLock);
        statusText = "Please wait for the previous extraction worker to finish before re-arming.";
        return false;
    }
    if (std::abs(hostSampleRate.load() - captureRate) > 0.5)
    {
        const juce::ScopedLock lock(dataLock);
        statusText = "Capture needs a 48 kHz Logic project. Change the project sample rate, then re-arm.";
        return false;
    }
    emittedSweep.resize(sweepSamples);
    const double duration = static_cast<double>(sweepSamples - 1) / captureRate;
    const double ratio = std::log(20000.0 / 20.0);
    const int fade = static_cast<int>(0.010 * captureRate);
    const float level = juce::Decibels::decibelsToGain(sweepLevelDb.load());
    for (int i = 0; i < sweepSamples; ++i)
    {
        const double t = i / static_cast<double>(captureRate);
        const double phase = juce::MathConstants<double>::twoPi * 20.0 * duration / ratio * (std::exp(ratio * t / duration) - 1.0);
        float env = 1.0f;
        if (i < fade) env *= 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * i / fade);
        if (i >= sweepSamples - fade) env *= 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * (sweepSamples - 1 - i) / fade);
        emittedSweep[static_cast<size_t>(i)] = level * env * static_cast<float>(std::sin(phase));
    }
    float emittedPeak = 0.0f;
    for (float sample : emittedSweep) emittedPeak = juce::jmax(emittedPeak, std::abs(sample));
    if (emittedPeak > 0.0f)
        for (float& sample : emittedSweep) sample *= level / emittedPeak;
    inverseSweep.resize(static_cast<size_t>(sweepSamples));
    for (int i = 0; i < sweepSamples; ++i)
    {
        const float compensation = std::exp(-static_cast<float>(ratio * i / (sweepSamples - 1)));
        inverseSweep[static_cast<size_t>(i)] = emittedSweep[static_cast<size_t>(sweepSamples - 1 - i)] * compensation;
    }
    recordedReturn.assign(static_cast<size_t>(leadSamples + sweepSamples + tailSamples), 0.0f);
    capturePosition.store(0);
    captureClipping.store(false);
    captureBlockOverflow.store(false);
    cancelExtraction.store(false);
    {
        const juce::ScopedLock lock(dataLock);
        resultIR.clear();
        statusText = "Capture armed. Output is silent until Start.";
    }
    captureState.store(CaptureState::Armed, std::memory_order_release);
    return true;
}
void IRCaptureLoadAudioProcessor::startCapture()
{
    if (captureState.load() == CaptureState::Armed)
    {
        capturePosition.store(0);
        captureState.store(CaptureState::Recording, std::memory_order_release);
        const juce::ScopedLock lock(dataLock);
        statusText = "Recording sweep and microphone return…";
    }
}
void IRCaptureLoadAudioProcessor::abortCapture(const juce::String& reason)
{
    const auto state = captureState.load();
    if (state == CaptureState::Loader) return;
    cancelExtraction.store(true);
    captureState.store(CaptureState::Error, std::memory_order_release);
    const juce::ScopedLock lock(dataLock);
    resultIR.clear();
    statusText = reason;
}
void IRCaptureLoadAudioProcessor::CaptureWorker::run()
{
    while (!threadShouldExit())
    {
        owner.extractionReady.wait(100);
        if (threadShouldExit()) break;
        if (owner.captureState.load(std::memory_order_acquire) == CaptureState::Processing)
            owner.extractCapture();
        else
            owner.extractionBusy.store(false, std::memory_order_release);
    }
}
void IRCaptureLoadAudioProcessor::extractCapture()
{
    std::vector<float> ret = recordedReturn;
    std::vector<float> inverse = inverseSweep;
    std::vector<float> extracted;
    juce::String error;
    const bool ok = !cancelExtraction.load() && decodeAndExtract(ret, inverse, leadSamples, captureRate, extracted, error);
    if (cancelExtraction.load())
    {
        extractionBusy.store(false, std::memory_order_release);
        return;
    }
    const juce::ScopedLock lock(dataLock);
    if (cancelExtraction.load() || captureState.load() != CaptureState::Processing)
    {
        extractionBusy.store(false, std::memory_order_release);
        return;
    }
    if (ok)
    {
        resultIR = std::move(extracted);
        statusText.clear();
        captureState.store(CaptureState::Ready, std::memory_order_release);
    }
    else
    {
        resultIR.clear();
        statusText = error;
        captureState.store(CaptureState::Error, std::memory_order_release);
    }
    extractionBusy.store(false, std::memory_order_release);
}

bool IRCaptureLoadAudioProcessor::decodeAndExtract(const std::vector<float>& recorded,
    const std::vector<float>& inverseSweep, int lead, int fs, std::vector<float>& result, juce::String& error)
{
    if (recorded.empty() || inverseSweep.empty()) { error = "No recorded return was captured."; return false; }
    float peak = 0.0f;
    double sumSquares = 0.0;
    for (float x : recorded)
    {
        if (!std::isfinite(x)) { error = "Capture contains invalid samples."; return false; }
        peak = juce::jmax(peak, std::abs(x));
        sumSquares += static_cast<double>(x) * x;
    }
    if (peak >= 0.999f) { error = "Capture clipped. Lower the return gain and try again."; return false; }
    if (peak < 0.001f || sumSquares / recorded.size() < 1.0e-9)
    { error = "Return is silent or too quiet. Check routing and raise the mic return gain."; return false; }

    // Linear convolution with the amplitude-compensated inverse of the exact emitted ESS.
    // Harmonic responses land before the main linear response and are excluded by the latency window below.
    const int needed = static_cast<int>(recorded.size() + inverseSweep.size() - 1);
    const int order = juce::roundToInt(std::ceil(std::log2(static_cast<double>(needed))));
    const int fftSize = 1 << order;
    juce::dsp::FFT fft(order);
    std::vector<float> x(static_cast<size_t>(fftSize * 2), 0.0f), y(static_cast<size_t>(fftSize * 2), 0.0f);
    std::copy(recorded.begin(), recorded.end(), y.begin());
    std::copy(inverseSweep.begin(), inverseSweep.end(), x.begin());
    fft.performRealOnlyForwardTransform(x.data());
    fft.performRealOnlyForwardTransform(y.data());
    for (int k = 0; k <= fftSize / 2; ++k)
    {
        const double frequency = static_cast<double>(k) * fs / fftSize;
        const size_t ri = static_cast<size_t>(2 * k), ii = ri + 1;
        if (frequency < 20.0 || frequency > 20000.0)
        { y[ri] = y[ii] = 0.0f; continue; }
        const float xr = x[ri], xi = x[ii], yr = y[ri], yi = y[ii];
        y[ri] = yr * xr - yi * xi;
        y[ii] = yr * xi + yi * xr;
    }
    fft.performRealOnlyInverseTransform(y.data());
    const int responseStart = lead + static_cast<int>(inverseSweep.size()) - 1;
    const int latencyLimit = fs / 4;
    float responsePeak = 0.0f;
    int peakIndex = -1;
    const int responseEnd = juce::jmin(responseStart + latencyLimit, fftSize);
    std::vector<float> responseLevels;
    responseLevels.reserve(static_cast<size_t>(juce::jmax(0, responseEnd - responseStart)));
    for (int i = responseStart; i < responseEnd; ++i)
    {
        responseLevels.push_back(std::abs(y[static_cast<size_t>(i)]));
        if (std::abs(y[static_cast<size_t>(i)]) > responsePeak)
        { responsePeak = std::abs(y[static_cast<size_t>(i)]); peakIndex = i; }
    }
    if (peakIndex < 0 || responsePeak < 1.0e-5f)
    { error = "No clear impulse response found in the first 250 ms. Check the send and return routing."; return false; }
    auto middle = responseLevels.begin() + static_cast<std::ptrdiff_t>(responseLevels.size() / 2);
    std::nth_element(responseLevels.begin(), middle, responseLevels.end());
    if (responsePeak < *middle * 8.0f)
    { error = "Return looks like noise rather than a clear impulse response. Check the mic and signal path."; return false; }
    const float onsetThreshold = responsePeak * 0.025f;
    int onset = peakIndex;
    while (onset > responseStart && std::abs(y[static_cast<size_t>(onset - 1)]) >= onsetThreshold) --onset;
    const int start = juce::jmax(0, onset - fs / 1000);
    if (start + maxIrSamples > fftSize || peakIndex >= responseStart + latencyLimit)
    { error = "The detected response is too late to keep the full 200 ms window."; return false; }
    result.resize(maxIrSamples);
    float irPeak = 0.0f;
    for (int i = 0; i < maxIrSamples; ++i)
    {
        result[static_cast<size_t>(i)] = y[static_cast<size_t>(start + i)];
        irPeak = juce::jmax(irPeak, std::abs(result[static_cast<size_t>(i)]));
    }
    if (irPeak < 1.0e-6f) { result.clear(); error = "The extracted response is too quiet to use."; return false; }
    for (float& s : result) s /= irPeak;
    const int fade = fs / 100;
    for (int i = 0; i < fade; ++i)
        result[result.size() - static_cast<size_t>(fade) + static_cast<size_t>(i)] *=
            0.5f + 0.5f * std::cos(juce::MathConstants<float>::pi * i / fade);
    return true;
}

juce::String IRCaptureLoadAudioProcessor::getCaptureStatus() const
{
    if (captureBlockOverflow.load()) return "Host audio block was too large. Capture aborted and stayed silent.";
    if (captureClipping.load()) return "Return clipped or contained invalid samples. Lower the return gain and try again.";
    const juce::ScopedLock lock(dataLock);
    return statusText;
}
float IRCaptureLoadAudioProcessor::getCaptureProgress() const
{
    if (captureState.load() == CaptureState::Armed) return 0.0f;
    if (captureState.load() == CaptureState::Processing || captureState.load() == CaptureState::Ready) return 1.0f;
    return juce::jlimit(0.0f, 1.0f, capturePosition.load() / static_cast<float>(leadSamples + sweepSamples + tailSamples));
}
juce::String IRCaptureLoadAudioProcessor::getRoutingNotice() const
{
    return "Disable interface direct monitoring into the reamp output. Do not bypass or remove this plugin while the hardware loop is active; host bypass can route around its silence protection.";
}
std::vector<float> IRCaptureLoadAudioProcessor::getResultWaveform() const
{
    const juce::ScopedLock lock(dataLock);
    if (resultIR.empty()) return {};
    std::vector<float> bins(240, 0.0f);
    for (int x = 0; x < 240; ++x)
    {
        const int a = x * static_cast<int>(resultIR.size()) / 240;
        const int z = juce::jmax(a + 1, (x + 1) * static_cast<int>(resultIR.size()) / 240);
        for (int i = a; i < juce::jmin(z, static_cast<int>(resultIR.size())); ++i)
            bins[static_cast<size_t>(x)] = juce::jmax(bins[static_cast<size_t>(x)], std::abs(resultIR[static_cast<size_t>(i)]));
    }
    return bins;
}
std::vector<float> IRCaptureLoadAudioProcessor::getResultSpectrum() const
{
    constexpr int fftOrder = 15;
    constexpr int fftSize = 1 << fftOrder;
    constexpr int points = 240;
    std::vector<float> samples;
    {
        const juce::ScopedLock lock(dataLock);
        samples = resultIR;
    }
    if (samples.empty()) return {};
    juce::dsp::FFT fft(fftOrder);
    std::vector<float> fftData(static_cast<size_t>(fftSize * 2), 0.0f);
    const int copied = juce::jmin(static_cast<int>(samples.size()), fftSize);
    for (int i = 0; i < copied; ++i)
    {
        const float window = copied > 1 ? 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * i / (copied - 1)) : 1.0f;
        fftData[static_cast<size_t>(i)] = samples[static_cast<size_t>(i)] * window;
    }
    fft.performRealOnlyForwardTransform(fftData.data());
    const int maxBin = fftSize / 2;
    std::vector<float> magnitudes(static_cast<size_t>(maxBin + 1), 0.0f);
    float maxMagnitude = 0.0f;
    for (int k = 1; k <= maxBin; ++k)
    {
        const float re = fftData[static_cast<size_t>(2 * k)];
        const float im = fftData[static_cast<size_t>(2 * k + 1)];
        magnitudes[static_cast<size_t>(k)] = std::sqrt(re * re + im * im);
        maxMagnitude = juce::jmax(maxMagnitude, magnitudes[static_cast<size_t>(k)]);
    }
    if (maxMagnitude <= 0.0f) return {};
    std::vector<float> db(static_cast<size_t>(points), -60.0f);
    for (int x = 0; x < points; ++x)
    {
        const double fraction = x / static_cast<double>(points - 1);
        const double frequency = 20.0 * std::pow(20000.0 / 20.0, fraction);
        const double halfBand = frequency * (std::pow(2.0, 1.0 / 24.0) - 1.0);
        const int first = juce::jmax(1, static_cast<int>(std::floor((frequency - halfBand) * fftSize / captureRate)));
        const int last = juce::jmin(maxBin, static_cast<int>(std::ceil((frequency + halfBand) * fftSize / captureRate)));
        double sumSquares = 0.0;
        int count = 0;
        for (int k = first; k <= last; ++k)
        {
            const double hz = static_cast<double>(k) * captureRate / fftSize;
            if (hz >= 20.0 && hz <= 20000.0)
            { const double m = magnitudes[static_cast<size_t>(k)]; sumSquares += m * m; ++count; }
        }
        const double magnitude = count > 0 ? std::sqrt(sumSquares / count) : 0.0;
        db[static_cast<size_t>(x)] = static_cast<float>(juce::jlimit(-60.0, 0.0,
            magnitude > 0.0 ? 20.0 * std::log10(magnitude / maxMagnitude) : -60.0));
    }
    return db;
}
bool IRCaptureLoadAudioProcessor::useResultInSlot(int slot)
{
    if (captureState.load() != CaptureState::Ready || slot < 0 || slot > 1) return false;
    std::vector<float> copy;
    {
        const juce::ScopedLock lock(dataLock);
        if (resultIR.empty()) return false;
        copy = resultIR;
    }
    juce::String error;
    return setIR(slot, copy.data(), static_cast<int>(copy.size()), captureRate, "Captured IR.wav", error);
}
bool IRCaptureLoadAudioProcessor::saveResultWav(const juce::File& file, juce::String& error) const
{
    const juce::ScopedLock lock(dataLock);
    if (resultIR.empty()) { error = "There is no completed capture to save."; return false; }
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (!stream) { error = "Could not create the WAV file."; return false; }
    const auto options = juce::AudioFormatWriterOptions()
        .withSampleRate(captureRate).withNumChannels(1).withBitsPerSample(32)
        .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream, options));
    if (!writer) { error = "Could not write a 32-bit float WAV."; return false; }
    juce::AudioBuffer<float> data(1, static_cast<int>(resultIR.size()));
    data.copyFrom(0, 0, resultIR.data(), static_cast<int>(resultIR.size()));
    if (!writer->writeFromAudioSampleBuffer(data, 0, data.getNumSamples())) { error = "WAV writing failed."; return false; }
    error.clear();
    return true;
}

bool IRCaptureLoadAudioProcessor::saveSlotWav(int slot, const juce::File& file, juce::String& error) const
{
    if (slot < 0 || slot > 1) { error = "Choose slot A or B."; return false; }
    std::vector<float> source;
    double sourceRate = 0.0;
    {
        const juce::ScopedLock lock(dataLock);
        const auto& data = slots[slot].ir;
        if (data.getNumSamples() == 0) { error = "This slot is empty."; return false; }
        sourceRate = slots[slot].sampleRate;
        source.assign(data.getReadPointer(0), data.getReadPointer(0) + data.getNumSamples());
    }
    const int outputCount = juce::jmax(1, juce::roundToInt(source.size() * 48000.0 / sourceRate));
    juce::AudioBuffer<float> data(1, outputCount);
    auto* output = data.getWritePointer(0);
    const double cutoff = juce::jmin(1.0, 48000.0 / sourceRate);
    constexpr int radius = 16;
    for (int i = 0; i < outputCount; ++i)
    {
        const double position = i * sourceRate / 48000.0;
        const int centre = static_cast<int>(std::floor(position));
        double weighted = 0.0, weightSum = 0.0;
        for (int tap = centre - radius + 1; tap <= centre + radius; ++tap)
        {
            if (tap < 0 || tap >= static_cast<int>(source.size())) continue;
            const double distance = position - tap;
            const double scaled = distance * cutoff;
            const double sinc = std::abs(scaled) < 1.0e-9 ? 1.0
                : std::sin(juce::MathConstants<double>::pi * scaled) / (juce::MathConstants<double>::pi * scaled);
            const double windowPosition = distance / radius;
            if (std::abs(windowPosition) >= 1.0) continue;
            const double window = 0.5 + 0.5 * std::cos(juce::MathConstants<double>::pi * windowPosition);
            const double weight = cutoff * sinc * window;
            weighted += source[static_cast<size_t>(tap)] * weight;
            weightSum += weight;
        }
        output[i] = weightSum != 0.0 ? static_cast<float>(weighted / weightSum) : 0.0f;
    }
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (!stream) { error = "Could not create the WAV file."; return false; }
    const auto options = juce::AudioFormatWriterOptions().withSampleRate(48000.0).withNumChannels(1)
        .withBitsPerSample(32).withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream, options));
    if (!writer) { error = "Could not write a 32-bit float WAV."; return false; }
    if (!writer->writeFromAudioSampleBuffer(data, 0, outputCount)) { error = "WAV writing failed."; return false; }
    error.clear();
    return true;
}

void IRCaptureLoadAudioProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    juce::MemoryOutputStream out(dest, false);
    out.writeInt(0x4952434c); out.writeInt(4);
    const auto state = captureState.load();
    out.writeInt(state == CaptureState::Loader ? 0 : 1); // restore any capture page state silently and unarmed
    out.writeFloat(sweepLevelDb.load());
    out.writeFloat(inputGainDb.load());
    out.writeFloat(outputGainDb.load());
    out.writeFloat(slotBOffsetMs.load());
    const juce::ScopedLock lock(dataLock);
    for (auto& slot : slots)
    {
        out.writeBool(slot.loaded.load()); out.writeBool(slot.enabled.load()); out.writeBool(slot.truncated.load()); out.writeFloat(slot.gainDb.load());
        out.writeDouble(slot.sampleRate); out.writeString(slot.name);
        const int count = slot.loaded.load() ? slot.ir.getNumSamples() : 0;
        out.writeInt(count);
        for (int i = 0; i < count; ++i) out.writeFloat(slot.ir.getSample(0, i));
    }
}
void IRCaptureLoadAudioProcessor::setStateInformation(const void* data, int size)
{
    juce::MemoryInputStream in(data, static_cast<size_t>(size), false);
    if (in.readInt() != 0x4952434c) return;
    const int version = in.readInt();
    if (version != 2 && version != 3 && version != 4) return;
    const bool savedSilent = in.readInt() != 0;
    sweepLevelDb.store(juce::jlimit(-60.0f, -6.0f, in.readFloat()));
    if (version >= 3)
    {
        inputGainDb.store(juce::jlimit(-24.0f, 24.0f, in.readFloat()));
        outputGainDb.store(juce::jlimit(-24.0f, 24.0f, in.readFloat()));
    }
    slotBOffsetMs.store(version >= 4 ? juce::jlimit(-2.0f, 2.0f, in.readFloat()) : 0.0f);
    for (int s = 0; s < 2; ++s)
    {
        const bool loaded = in.readBool(), enabled = in.readBool(), truncated = in.readBool();
        const float gain = juce::jlimit(-60.0f, 6.0f, in.readFloat());
        const double rate = in.readDouble();
        const auto name = in.readString();
        const int count = in.readInt();
        if (count < 0 || count > 200000) return;
        std::vector<float> samples(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) samples[static_cast<size_t>(i)] = in.readFloat();
        slots[s].enabled.store(enabled); slots[s].gainDb.store(gain);
        if (loaded && count > 0)
        {
            juce::String error;
            setIR(s, samples.data(), count, rate, name, error);
            slots[s].truncated.store(truncated);
        }
        else
        {
            const juce::ScopedLock lock(dataLock);
            slots[s].ir.setSize(1, 0);
            slots[s].name.clear();
            slots[s].loaded.store(false, std::memory_order_release);
            slots[s].truncated.store(false);
        }
    }
    captureState.store(savedSilent ? CaptureState::Error : CaptureState::Loader);
    const juce::ScopedLock lock(dataLock);
    resultIR.clear();
    statusText = savedSilent ? "Capture session restored silent and unarmed. Choose Enable Loader to return to playback." : "Session restored.";
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new IRCaptureLoadAudioProcessor();
}
