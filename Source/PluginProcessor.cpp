#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

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
    inputGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(inputGainDb.load()));
    outputGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(outputGainDb.load()));
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
    setLatencySamples(juce::jmax(slots[0].convolution.getLatency(), slots[1].convolution.getLatency()));
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
        const int length = static_cast<int>(recordedReturn.size());
        int pos = capturePosition.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            float sample = 0.0f;
            if (state == CaptureState::Recording && pos < length)
            {
                    const float rawReturn = in[i];
                    const float ret = rawReturn * inputGain.getNextValue();
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
        if (!slot.loaded.load(std::memory_order_acquire) || !slot.enabled.load()) continue;
        auto& work = s == 0 ? workA : workB;
        auto& block = s == 0 ? blockA : blockB;
        slot.convolution.process(juce::dsp::ProcessContextReplacing<float>(block));
        auto* source = work.getWritePointer(0);
        const float targetGain = juce::Decibels::decibelsToGain(slot.gainDb.load());
        slot.gain.setTargetValue(targetGain);
        for (int i = 0; i < n; ++i)
        {
            out[i] += source[i] * slot.gain.getNextValue();
            finalPeak = juce::jmax(finalPeak, std::abs(out[i]));
        }
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
        statusText = "Capture ready. Assign to A or B, or save as WAV.";
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

void IRCaptureLoadAudioProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    juce::MemoryOutputStream out(dest, false);
    out.writeInt(0x4952434c); out.writeInt(3);
    const auto state = captureState.load();
    out.writeInt(state == CaptureState::Loader ? 0 : 1); // restore any capture page state silently and unarmed
    out.writeFloat(sweepLevelDb.load());
    out.writeFloat(inputGainDb.load());
    out.writeFloat(outputGainDb.load());
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
    if (version != 2 && version != 3) return;
    const bool savedSilent = in.readInt() != 0;
    sweepLevelDb.store(juce::jlimit(-60.0f, -6.0f, in.readFloat()));
    if (version >= 3)
    {
        inputGainDb.store(juce::jlimit(-24.0f, 24.0f, in.readFloat()));
        outputGainDb.store(juce::jlimit(-24.0f, 24.0f, in.readFloat()));
    }
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
