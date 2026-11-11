function DAQ_Validation_Viewer
%DAQ_VALIDATION_VIEWER Compare the DAQ load cell with the hydraulic press log.
%
%   DAQ_Validation_Viewer
%
%   Two files are chosen in dialogs. Nothing is read from a fixed path.
%     1. Press CSV: Time (s), Force (lbf), Displacement (in). Header rows are skipped.
%     2. DAQ file, either:
%        - a NAND .BIN file (downloaded from the board; decoded here, load cell records only), or
%        - the live_data_loadcell_<time>.csv file of the calibration app (Time, mV/V).
%
%   Top plot: the load cell signal (mV/V) for the whole DAQ file, decimated to the plot rate (default 50 Hz).
%     The points are averaged over the full-rate data, so the decimation does not change them.
%     The green line (zero point) and the magenta line (max point) move with their sliders.
%     Each point is the average of the signal over the "Average" width around it.
%     The signal is scaled to lbf by the straight line through
%       zero point -> 0 lbf    and    max point -> the max load you type.
%   Bottom plot: DAQ force (lbf) and press force (lbf) inside the window.
%     Press time + press start + fine offset = DAQ time. The press start slider spans the whole DAQ file
%     (set it to where the press test begins, e.g. 300 s after 5 minutes of idling). The fine offset is +-60 s.
%     The two window sliders select the part of the DAQ data that is compared.

% ---- choose and read the files
[pf, pp] = uigetfile({'*.csv', 'Press CSV (*.csv)'}, 'Select the PRESS csv (Time, Force, Displacement)');
if isequal(pf, 0), return; end
try
    press = read_press(fullfile(pp, pf));
catch err
    errordlg(err.message, 'Press file');
    return
end

[df, dp] = uigetfile({'*.bin;*.csv', 'DAQ data (*.bin, *.csv)'; '*.bin', 'NAND .BIN (*.bin)'; ...
    '*.csv', 'Live load cell CSV (*.csv)'}, 'Select the DAQ file (NAND .BIN or live_data_loadcell CSV)', pp);
if isequal(df, 0), return; end
try
    daq = read_daq(fullfile(dp, df));
catch err
    errordlg(err.message, 'DAQ file');
    return
end

t0 = daq.t(1);
t1 = daq.t(end);
span = t1 - t0;
ylo = min(daq.x);
yhi = max(daq.x);
pad = 0.05 * (yhi - ylo);
ylo = ylo - pad;
yhi = yhi + pad;

% ---- state: every slider and field writes here through set_val
P = struct('zero', t0 + 0.05 * span, 'maxp', t0 + 0.95 * span, 'avgw', 1.0, ...
           'lmax', 2200, 'rate', 50, 'ws', t0, 'we', t1, 'start', t0, 'off', 0);
H = struct();
cacheRate = NaN;   % plot rate of the decimated DAQ data dt_t, dt_x
dt_t = [];
dt_x = [];

% ---- window layout
fig = uifigure('Name', 'DAQ vs press validation', 'Position', [60 60 1350 820]);
root = uigridlayout(fig, [1 2]);
root.ColumnWidth = {320, '1x'};
root.Padding = [10 10 10 10];

cg = uigridlayout(root, [9 1]);
cg.RowHeight = {55, 55, 100, 55, 55, 55, 55, 40, 250};
cg.RowSpacing = 8;
cg.Scrollable = 'on';

[sZ, H.lZ] = slider_group(cg, [t0 t1], P.zero);
sZ.ValueChangingFcn = @(~, e) set_val('zero', e.Value);
sZ.ValueChangedFcn = @(~, e) set_val('zero', e.Value);

[sM, H.lM] = slider_group(cg, [t0 t1], P.maxp);
sM.ValueChangingFcn = @(~, e) set_val('maxp', e.Value);
sM.ValueChangedFcn = @(~, e) set_val('maxp', e.Value);

g3 = uigridlayout(cg, [3 2]);
g3.RowHeight = {'fit', 'fit', 'fit'};
g3.ColumnWidth = {'1x', 90};
g3.Padding = [0 0 0 0];
uilabel(g3, 'Text', 'Average around points (s)');
uieditfield(g3, 'numeric', 'Value', P.avgw, 'Limits', [0.01 span], ...
    'ValueChangedFcn', @(s, ~) set_val('avgw', s.Value));
uilabel(g3, 'Text', 'Max load (lbf)');
uieditfield(g3, 'numeric', 'Value', P.lmax, 'Limits', [0 Inf], ...
    'ValueChangedFcn', @(s, ~) set_val('lmax', s.Value));
uilabel(g3, 'Text', 'Plot rate (Hz)');
uieditfield(g3, 'numeric', 'Value', P.rate, 'Limits', [1 1000], ...
    'ValueChangedFcn', @(s, ~) set_val('rate', s.Value));

[sWs, H.lWs] = slider_group(cg, [t0 t1], P.ws);
sWs.ValueChangingFcn = @(~, e) set_val('ws', e.Value);
sWs.ValueChangedFcn = @(~, e) set_val('ws', e.Value);

[sWe, H.lWe] = slider_group(cg, [t0 t1], P.we);
sWe.ValueChangingFcn = @(~, e) set_val('we', e.Value);
sWe.ValueChangedFcn = @(~, e) set_val('we', e.Value);

[sS, H.lS] = slider_group(cg, [t0 t1], P.start);
sS.ValueChangingFcn = @(~, e) set_val('start', e.Value);
sS.ValueChangedFcn = @(~, e) set_val('start', e.Value);

[sO, H.lO] = slider_group(cg, [-60 60], P.off);
sO.ValueChangingFcn = @(~, e) set_val('off', e.Value);
sO.ValueChangedFcn = @(~, e) set_val('off', e.Value);

uibutton(cg, 'Text', 'Auto-align press (min RMS in window)', 'ButtonPushedFcn', @(~, ~) auto_align());

H.info = uitextarea(cg, 'Editable', 'off', 'FontName', 'Courier New', 'FontSize', 12, 'Value', {''});

pg = uigridlayout(root, [2 1]);
pg.RowHeight = {'1x', '1x'};
H.ax1 = uiaxes(pg);
H.ax2 = uiaxes(pg);

% ---- top plot: DAQ signal, the two points, the window
ax = H.ax1;
hold(ax, 'on'); grid(ax, 'on');
H.shade = patch(ax, [t0 t1 t1 t0], [ylo ylo yhi yhi], [0.9 0.9 0.2], ...
    'FaceAlpha', 0.25, 'EdgeColor', 'none');
H.dline = line(ax, NaN, NaN, 'Color', [0.2 0.4 0.8]);
H.lineZ = line(ax, [t0 t0], [ylo yhi], 'Color', [0 0.6 0], 'LineWidth', 2);
H.lineM = line(ax, [t0 t0], [ylo yhi], 'Color', [0.8 0 0.6], 'LineWidth', 2);
xlim(ax, [t0 t1]);
ylim(ax, [ylo yhi]);
xlabel(ax, 'DAQ time (s)');
ylabel(ax, 'Load cell (mV/V)');
title(ax, 'DAQ signal (green = zero point, magenta = max point, yellow = window)');

% ---- bottom plot: DAQ force against press force
ax = H.ax2;
hold(ax, 'on'); grid(ax, 'on');
H.dq = line(ax, NaN, NaN, 'Color', [0.2 0.4 0.8], 'LineWidth', 1.5, 'DisplayName', 'DAQ (lbf)');
H.pr = line(ax, press.t, press.F, 'Color', [0.85 0.2 0.2], 'LineWidth', 1.5, 'DisplayName', 'Press (lbf)');
legend(ax, 'show', 'Location', 'northwest');
xlabel(ax, 'DAQ time (s)');
ylabel(ax, 'Force (lbf)');
title(ax, 'DAQ force vs press force');

refresh();

    % ================= nested functions =================

    function [sl, lb] = slider_group(parent, lims, val)
        g = uigridlayout(parent, [2 1]);
        g.RowHeight = {'fit', 'fit'};
        g.Padding = [0 0 0 0];
        g.RowSpacing = 2;
        lb = uilabel(g, 'Text', '');
        sl = uislider(g, 'Limits', lims, 'Value', val);
    end

    function set_val(name, v)
        P.(name) = v;
        refresh();
    end

    function auto_align()
        % Finds the press shift S (press start + fine offset) with the smallest RMS difference between the DAQ force
        % and the press force, inside the selected window. The scale comes from the zero and max points.
        ws = P.ws;
        we = max(P.we, ws + 0.01);
        xz = point_value(P.zero);
        xm = point_value(P.maxp);
        if ~(isfinite(xz) && isfinite(xm)) || xm == xz
            H.info.Value = {'Pick two different zero and max points first.'};
            return
        end
        k = P.lmax / (xm - xz);
        m = dt_t >= ws & dt_t <= we;      % dt_t, dt_x: the decimated DAQ data of the current plot rate
        tw = dt_t(m);
        Fd = (dt_x(m) - xz) * k;
        if numel(tw) < 10
            H.info.Value = {'The window has too few samples. Widen it.'};
            return
        end

        % coarse search over every shift that puts the press data over the window, then a fine search
        grid = (ws - max(press.t)):0.25:(we - min(press.t));
        if isempty(grid)
            H.info.Value = {'The press data cannot overlap this window.'};
            return
        end
        c = arrayfun(@(s) align_cost(s, tw, Fd), grid);
        [cmin, ib] = min(c);
        if ~isfinite(cmin)
            H.info.Value = {'No shift gives at least half the window overlapping the press data.'};
            return
        end
        a = grid(max(ib - 1, 1));
        b = grid(min(ib + 1, numel(grid)));
        if b > a
            S = fminbnd(@(s) align_cost(s, tw, Fd), a, b);
        else
            S = grid(ib);
        end

        % the press start slider covers the DAQ file; the fine offset takes what is left (+-60 s)
        P.start = min(max(S, t0), t1);
        P.off = max(min(S - P.start, 60), -60);
        sS.Value = P.start;
        sO.Value = P.off;
        refresh();
        H.info.Value = [H.info.Value; {sprintf('Auto-align: press start %.2f s, fine offset %+.2f s, RMS %.2f lbf', ...
            P.start, P.off, align_cost(P.start + P.off, tw, Fd))}];
    end

    function c = align_cost(s, tw, Fd)
        % RMS difference for the press shifted by s (s = press start + fine offset). Inf if under half the window overlaps.
        Fp = interp1(press.t + s, press.F, tw, 'linear', NaN);
        ok = isfinite(Fp);
        if nnz(ok) < 0.5 * numel(tw)
            c = Inf;
        else
            c = sqrt(mean((Fd(ok) - Fp(ok)) .^ 2));
        end
    end

    function v = point_value(tc)
        % mean DAQ signal around time tc, over the Average width
        sel = abs(daq.t - tc) <= P.avgw / 2;
        if any(sel)
            v = mean(daq.x(sel));
        else
            v = interp1(daq.t, daq.x, tc, 'nearest', NaN);
        end
    end

    function refresh()
        ws = P.ws;
        we = max(P.we, ws + 0.01);

        H.lZ.Text = sprintf('Zero point: %.2f s', P.zero);
        H.lM.Text = sprintf('Max point: %.2f s', P.maxp);
        H.lWs.Text = sprintf('Window start: %.2f s', ws);
        H.lWe.Text = sprintf('Window end: %.2f s', we);
        H.lS.Text = sprintf('Press start in DAQ file: %.2f s', P.start);
        H.lO.Text = sprintf('Fine press offset: %+.2f s', P.off);

        H.lineZ.XData = [P.zero P.zero];
        H.lineM.XData = [P.maxp P.maxp];
        H.shade.XData = [ws we we ws];

        xz = point_value(P.zero);
        xm = point_value(P.maxp);
        if ~(isfinite(xz) && isfinite(xm)) || xm == xz
            H.info.Value = {'Pick two different points inside the DAQ data.'};
            return
        end

        if P.rate ~= cacheRate
            [dt_t, dt_x] = decimate_bins(daq.t, daq.x, 1 / P.rate);
            cacheRate = P.rate;
        end
        H.dline.XData = dt_t;
        H.dline.YData = dt_x;

        k = P.lmax / (xm - xz);                 % lbf per mV/V
        m = dt_t >= ws & dt_t <= we;
        tw = dt_t(m);
        Fd = (dt_x(m) - xz) * k;

        pt = press.t + P.start + P.off;         % press time moved onto the DAQ clock
        Fp = interp1(pt, press.F, tw, 'linear', NaN);
        kp = pt >= ws & pt <= we;
        Fpw = press.F(kp);

        H.dq.XData = tw;
        H.dq.YData = Fd;
        H.pr.XData = pt;
        H.pr.YData = press.F;
        H.ax2.XLim = [ws we];
        allv = [Fd; Fpw];
        if ~isempty(allv)
            pad2 = 0.05 * max(max(allv) - min(allv), 1);
            H.ax2.YLim = [min(allv) - pad2, max(allv) + pad2];
        end

        txt = {sprintf('Zero: %.4f mV/V (avg at %.2f s)', xz, P.zero), ...
               sprintf('Max : %.4f mV/V (avg at %.2f s) = %.1f lbf', xm, P.maxp, P.lmax), ...
               sprintf('Scale: %.2f lbf per mV/V', k), ...
               sprintf('Window: %.2f to %.2f s', ws, we)};

        ok = isfinite(Fp);
        if any(ok)
            d = Fd(ok) - Fp(ok);
            tok = tw(ok);
            [mx, im] = max(abs(d));
            txt = [txt, {sprintf('Overlap: %d samples, %.2f s', nnz(ok), tok(end) - tok(1)), ...
                         sprintf('Bias (DAQ - press): %+.2f lbf', mean(d)), ...
                         sprintf('RMS difference: %.2f lbf', sqrt(mean(d .^ 2))), ...
                         sprintf('Max |difference|: %.2f lbf at %.2f s', mx, tok(im))}];
        else
            txt = [txt, {'No press data overlaps this window (check the offset).'}];
        end

        if ~isempty(Fd)
            [pd, ipd] = max(Fd);
            txt = [txt, {sprintf('Peak DAQ  : %.1f lbf at %.2f s', pd, tw(ipd))}];
        end
        if ~isempty(Fpw)
            tpk = pt(kp);
            [pk, ipk] = max(Fpw);
            txt = [txt, {sprintf('Peak press: %.1f lbf at %.2f s', pk, tpk(ipk))}];
        end
        H.info.Value = txt(:);
    end
end

function p = read_press(file)
% Time (s), Force (lbf) rows. The header and unit rows have no numbers and are skipped.
txt = strrep(fileread(file), '"', '');
lines = splitlines(txt);
rows = nan(numel(lines), 2);
for k = 1:numel(lines)
    v = str2double(split(lines{k}, ','));
    if numel(v) >= 2
        rows(k, :) = v(1:2)';
    end
end
ok = all(isfinite(rows), 2);
if ~any(ok)
    error('No Time,Force rows found in %s.', file);
end
p.t = rows(ok, 1);
p.F = rows(ok, 2);
[p.t, si] = sort(p.t);
p.F = p.F(si);
[p.t, iu] = unique(p.t, 'stable');   % interp1 needs unique sample times
p.F = p.F(iu);
end

function [td, xd] = decimate_bins(t, x, dt)
% Average the samples in bins of dt seconds. Each bin gives one point at its mean time and mean value.
t = t(:);
x = x(:);
b = floor((t - t(1)) / dt) + 1;
cnt = accumarray(b, 1);
sx = accumarray(b, x);
st = accumarray(b, t);
k = cnt > 0;
td = st(k) ./ cnt(k);
xd = sx(k) ./ cnt(k);
end

function d = read_daq(file)
% d.t (s from the first sample) and d.x (load cell, mV/V) from a .BIN or a live_data_loadcell CSV.
[~, ~, ext] = fileparts(file);
if strcmpi(ext, '.bin')
    d = read_bin_lc(file);
else
    d = read_live_lc(file);
end
end

function d = read_live_lc(file)
% live_data_loadcell_<time>.csv: "Time,mV/V". The PT file (live_data_<time>.csv, "Time,Voltage") is refused.
fid = fopen(file, 'r');
if fid == -1
    error('Cannot open %s.', file);
end
header = fgetl(fid);
c = textscan(fid, '%s %f', 'Delimiter', ',');   % a line being written may be cut: textscan stops there
fclose(fid);
if ~contains(header, 'mV/V')
    error('This is not a load cell file (header "%s"). Pick live_data_loadcell_<time>.csv.', header);
end
m = min(numel(c{1}), numel(c{2}));
tm = datetime(c{1}(1:m), 'InputFormat', 'yyyy-MM-dd HH:mm:ss.SSS');
ok = ~isnat(tm) & isfinite(c{2}(1:m));
tm = tm(ok);
if isempty(tm)
    error('No samples in %s.', file);
end
d.t = seconds(tm - tm(1));
d.x = c{2}(1:m);
d.x = d.x(ok);
d.t = d.t(:);
d.x = d.x(:);
end

function d = read_bin_lc(file)
% NAND .BIN (firmware record.h): 8-byte records, little endian. Load cell records have bit 7 of byte 8 set
% (and the file has a load cell: PT-only files have no load cell records). META records (time >= 0xFFFFFFF0)
% are the calibration and are not samples. Same decoding as the calibration app's loadRecords.
fid = fopen(file, 'r');
if fid == -1
    error('Cannot open %s.', file);
end
bytes = fread(fid, inf, 'uint8=>uint8');
fclose(fid);
n = floor(numel(bytes) / 8);
if n == 0
    error('%s is empty.', file);
end
r = reshape(bytes(1:n * 8), 8, n);
ts = double(typecast(reshape(r(1:4, :), [], 1), 'uint32'));
isMeta = ts >= 4294967280;                       % 0xFFFFFFF0
r = r(:, ~isMeta);
ts = ts(~isMeta);
isLc = bitand(r(8, :)', 128) ~= 0;
% In a file of the firmware before the load cell, that bit is a seq bit: it changes once in 32768 records.
if sum(diff(isLc) ~= 0) * 16384 < numel(isLc)
    isLc(:) = false;
end
if ~any(isLc)
    error('%s has no load cell records (PT-only file).', file);
end

raw = double(r(5, isLc))' + 256 * double(r(6, isLc))' + 65536 * double(r(7, isLc))';
raw(raw >= 2^23) = raw(raw >= 2^23) - 2^24;
tl = ts(isLc);
dt = diff(tl);
dt(dt < 0) = dt(dt < 0) + 2^32;                  % the timestamp wraps after 71 min
t = [0; cumsum(dt)] / 1e6;
if any(~isLc)
    % PT and load cell share one clock; time 0 is the first sample of the file (as the calibration app does)
    firstPt = ts(find(~isLc, 1));
    firstLc = tl(1);
    dd = mod(firstLc - firstPt + 2^31, 2^32) - 2^31;   % us from the first PT sample to the first load cell sample
    t = t + max(dd, 0) / 1e6;
end
d.t = t(:);
d.x = raw(:) * (1000 / 2^31);                    % counts -> mV/V
end
