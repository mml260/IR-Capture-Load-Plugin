function [ir, info] = extract_ir(recorded, inverse, leadSamples, fs)
%EXTRACT_IR Matched exponential-sweep extraction for the plugin reference.
% Applies zero-padded linear convolution, excludes earlier harmonic arrivals,
% searches 0..250 ms of round-trip latency, keeps 1 ms pre-onset and 200 ms,
% then peak-normalizes and fades the final 10 ms.
if nargin < 4, fs = 48000; end
if nargin < 3, leadSamples = round(0.25*fs); end
recorded = double(recorded(:)); inverse = double(inverse(:));
if isempty(recorded) || isempty(inverse), error('No recorded return or inverse sweep.'); end
if any(~isfinite(recorded)), error('Capture contains invalid samples.'); end
peak = max(abs(recorded));
if peak >= 0.999, error('Capture clipped. Lower return gain and retry.'); end
if peak < 1e-3 || mean(recorded.^2) < 1e-9
    error('Return is silent or too quiet. Check routing and return gain.');
end

% FFT length is large enough for linear convolution; no circular tail wrap.
nfft = 2^nextpow2(numel(recorded) + numel(inverse) - 1);
f = (0:nfft-1)' * fs/nfft;
f = min(f, fs-f);
product = fft(recorded,nfft) .* fft(inverse,nfft);
product(f < 20 | f > 20000) = 0;
response = ifft(product, 'symmetric');
responseStart = leadSamples + numel(inverse); % MATLAB 1-based location of zero-latency response
latencySamples = round(0.25*fs);
last = min(numel(response), responseStart + latencySamples - 1);
if last < responseStart, error('No response was recorded in the latency search window.'); end
levels = abs(response(responseStart:last));
[responsePeak, localPeak] = max(levels);
peakIndex = responseStart + localPeak - 1;
if responsePeak < 1e-5, error('No clear impulse response found in the first 250 ms.'); end
if responsePeak < 8*median(levels)
    error('Return looks like noise rather than a clear impulse response.');
end
threshold = 0.025 * responsePeak;
onset = peakIndex;
while onset > responseStart && abs(response(onset-1)) >= threshold
    onset = onset - 1;
end
start = max(1, onset - round(0.001*fs));
N = round(0.2*fs);
if start + N - 1 > numel(response)
    error('The detected response is too late to retain the full 200 ms window.');
end
ir = response(start:start+N-1);
ir = ir / max(abs(ir));
fadeN = round(0.010*fs);
ir(end-fadeN+1:end) = ir(end-fadeN+1:end) .* ...
    (0.5 + 0.5*cos(pi*(0:fadeN-1)'/fadeN));
info = struct('peakIndex',peakIndex,'onsetIndex',onset,'startIndex',start, ...
    'latencySamples',peakIndex-responseStart,'sampleRate',fs, ...
    'preOnsetSamples',onset-start,'normalizedPeak',max(abs(ir)));
end
