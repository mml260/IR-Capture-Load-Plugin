# IR Capture + Load

A small macOS Audio Unit v2 effect for Logic Pro. It accepts mono audio, convolves it through two mono IR slots, and can capture a clean linear amp/cabinet/microphone response through Logic's existing input/output routing.

## Build

Requirements: macOS, Xcode command line tools, CMake, and a JUCE 8 checkout. This repository contains the plugin source, not JUCE itself or a prebuilt signed installer.

```sh
cmake -S . -B build -G 'Unix Makefiles' \
  -DJUCE_DIR=/path/to/JUCE \
  -DCMAKE_C_COMPILER="$(xcrun --find clang)" \
  -DCMAKE_CXX_COMPILER="$(xcrun --find clang++)" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -- -j4
```

The AU bundle is `build/IRCaptureLoad_artefacts/Release/AU/IR Capture + Load.component`. Copy it to `~/Library/Audio/Plug-Ins/Components/` and rescan Audio Units in Logic. The plugin identifies as type `aufx`, subtype `IRLd`, manufacturer `IRCp`.

## Loader

1. Insert **IR Capture + Load** on a mono audio track. The **Input** knob adjusts the signal before both IR slots; **Output** adjusts the summed Loader output. Both are 0 dB by default and range from -24 to +24 dB.
2. Load a mono WAV into A or B. PCM and floating-point WAV are supported. Files are capped at 200 ms; longer files get a short end fade. Imported IRs keep their level and leading delay.
3. Each slot has its own level (-60 to +6 dB) and enable switch. Empty or disabled slots are silent. **Export WAV** saves that slot's IR as mono, 48 kHz, 32-bit float WAV; it does not include the phase alignment shift.
4. The Loader displays the first 10 ms of both IRs. **Auto Align** estimates a time shift for B against A; use the slider for manual adjustment within ±2 ms. The project stores the offset along with both IR sample payloads and their names/settings, so session recall does not depend on the original WAV path.

## Capture in Logic

This captures a linear response. Keep the amp clean; distortion and other level-dependent behavior are outside this IR model.

1. Set the Logic project to **48 kHz**. Capture is disabled at other host rates. In Logic, change the project sample rate in **File > Project Settings > Audio**.
2. Select your audio interface as Logic's input and output device. The plugin uses Logic's track I/O and does not open or address the interface directly. On the Quad Cortex's standard USB map, host Outputs 5–8 can feed USB inputs on The Grid, while processed QC Outputs 1/2 return to the host on USB Inputs 3/4. Confirm routing in QC I/O settings because USB routing is configurable.
3. On one mono audio track, select that mic input. Insert the plugin and open its Capture page. Set the track fader to unity and remove other inserts/sends that affect the measurement path.
4. Click **Arm Capture** before enabling the hardware send. Arming switches the plugin output to silence. Enable Logic input monitoring so the plugin receives the return.
5. Select the actual mono output mapping in Logic. Keep monitor speakers disconnected or muted from the send and keep other playback off that output.
6. Start with a low interface/reamp output. The plugin sweep defaults to -20 dBFS peak (adjustable -60 to -6); the Loader Output knob does not change capture send level. During recording, aim for the green return-meter range of -12 to -6 dBFS without clipping. The Input knob also affects the captured return and can cause digital clipping if set too high. The digital sweep setting is not a calibrated voltage.
7. Click **Start**. The plugin records a 250 ms lead-in, one 262,144-sample exponential sweep, and a 1 s tail. Extraction runs off the audio thread. A valid result displays its impulse waveform and a log-frequency FFT magnitude response (20 Hz–20 kHz, 1/12-octave smoothing, relative dB). Assign it directly to A/B or save as mono 48 kHz 32-bit float WAV.
8. After Ready, Error, or Abort, the plugin stays silent. Before clicking **Enable Loader**, change the Logic track input back to the guitar/DI source and output back to the normal monitor bus, then confirm the prompt.

Disable interface direct monitoring into the reamp output. Do not bypass or remove the plugin while this loop is connected: host bypass can route around the plugin's silence protection. The plugin never opens the audio interface itself; configure Quad Cortex routing in Logic and on the QC.

## Capture DSP and verification

`DSP/generate_sweep.m` and `DSP/extract_ir.m` preserve the MATLAB reference implementation; `DSP/validate_capture.m` synthesizes a delayed FIR for shape and polarity checks. The JUCE plugin currently uses the corresponding C++ exponential sweep and amplitude-compensated inverse. It performs zero-padded linear convolution, searches a 0–250 ms return-latency window after the known lead-in, excludes the earlier harmonic-response region, retains 1 ms before onset, and peak-normalizes a 200 ms window with a 10 ms end fade.

MATLAB code generation is **not integrated** into the AU. MATLAB R2026b is installed but exits at startup on this arm64 machine with a Qt processor-feature error (`neon`); the installed R2024b and R2025b batch launches also exit before executing commands. Toolbox licenses therefore could not be checked. The readable MATLAB source is included so it can be run once MATLAB starts successfully. Sweep generation and extraction currently run in JUCE, so Logic does not require MATLAB.

## Limits and checks

- Mono in/out only; AU v2 only. No VST3, dry/wet, EQ, full phase correction, nonlinear modeling, averaging, or calibration. Loader alignment is a time shift of slot B, not all-pass phase correction.
- Keep the measured amp clean. Hardware round-trip delay over 250 ms and returns with no clear response are rejected. Input clipping, invalid samples, host reset/sample-rate change, and abort keep capture silent and preserve previous slot IRs.
- Physical reamp/amp capture has not been performed. Confirm the interface and Quad Cortex port routing on the actual setup.
- Build verified with Xcode 26.2 and CMake on Apple silicon; the development AU is installed in `~/Library/Audio/Plug-Ins/Components/`.
- `auval -v aufx IRLd IRCp` could not validate the component in this environment. The same `auval` invocation also failed to discover a built-in Apple AU, so this does not establish whether this plugin itself passes validation.
- Logic's Audio Unit scan log listed this component, but a full insert/open and hardware-routing check has not been completed.
- `DSP/validate_capture.m` has not run because MATLAB fails during startup here. Run it when MATLAB starts successfully; an equivalent numerical delayed-FIR/noise check was exercised during development, but it is not a MATLAB or in-host test.
