function [sweep, inverse, info] = generate_sweep(levelDbfs)
%GENERATE_SWEEP Build the exact 48 kHz exponential sweep used by the plugin.
%   levelDbfs is peak dBFS, clamped to the product range [-60, -6].
if nargin < 1, levelDbfs = -20; end
fs = 48000;
N = 262144;
f1 = 20;
f2 = 20000;
fadeN = round(0.010 * fs);
levelDbfs = min(-6, max(-60, levelDbfs));

n = (0:N-1)';
T = (N-1) / fs;
L = log(f2/f1);
t = n / fs;
phase = 2*pi*f1*T/L .* (exp(L*t/T) - 1);
env = ones(N,1);
fade = 0.5 - 0.5*cos(pi*(0:fadeN-1)'/fadeN);
env(1:fadeN) = fade;
env(end-fadeN+1:end) = flipud(fade);
sweep = 10^(levelDbfs/20) .* env .* sin(phase);
sweep = sweep .* (10^(levelDbfs/20) / max(abs(sweep)));

% Matched inverse: reverse the actual faded/levelled waveform and apply
% exponential amplitude compensation. This preserves the chosen sweep level.
compensation = exp(-L*n/(N-1));
inverse = flipud(sweep) .* compensation;
info = struct('sampleRate',fs,'samples',N,'startHz',f1,'endHz',f2, ...
    'levelDbfsPeak',levelDbfs,'fadeSamples',fadeN,'leadSeconds',0.25, ...
    'tailSeconds',1.0,'irSeconds',0.2);
end
