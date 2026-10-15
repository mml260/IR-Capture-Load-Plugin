function validate_capture
%VALIDATE_CAPTURE Synthetic delayed-FIR check for the reference DSP.
fs = 48000;
lead = round(0.25*fs);
tail = fs;
[sweep,inverse] = generate_sweep(-20);

% A linear FIR with a clear onset, several signed reflections, and delay.
h = zeros(round(0.15*fs),1);
h(129) = 1.0;
h(421) = 0.45;
h(1800) = -0.22;
h(6200) = 0.10;
reference = [zeros(lead,1); sweep; zeros(tail,1)];
recorded = conv(reference,h);
recorded = recorded(1:lead+numel(sweep)+tail);
[actual,info] = extract_ir(recorded,inverse,lead,fs);

responseStart = lead + numel(inverse);
expectedStart = max(1,info.onsetIndex-responseStart+1-info.preOnsetSamples);
expected = zeros(round(0.2*fs),1);
expected(1:numel(h)-expectedStart+1) = h(expectedStart:end);
expected = expected / max(abs(expected));
fadeN = round(0.01*fs);
expected(end-fadeN+1:end) = expected(end-fadeN+1:end) .* ...
    (0.5+0.5*cos(pi*(0:fadeN-1)'/fadeN));

fprintf('Recovered latency estimate: %d samples\n',info.latencySamples);
fprintf('Recovered IR peak: %.6f\n',max(abs(actual)));
fprintf('Shape error (max abs): %.6f\n',max(abs(actual-expected)));
directIndex = 129-expectedStart+1;
assert(actual(directIndex) > 0, 'Direct impulse polarity was not preserved.');
assert(max(abs(actual-expected)) < 0.25, 'Recovered FIR shape differs by more than the band-limited tolerance.');
assert(info.latencySamples >= 0 && info.latencySamples <= round(.25*fs), 'Latency outside search window.');
end
