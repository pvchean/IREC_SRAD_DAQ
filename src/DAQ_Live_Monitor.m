function DAQ_Live_Monitor()
%DAQ_LIVE_MONITOR Live view of the telemetry in the safety bunker (both sensors, link state, stale warning).
%
%   DAQ_Live_Monitor
%
%   The DAQ in the bunker is in DOWNLOAD mode (set it in DAQ_Calibration_App: WiFi... button, then reboot the DAQ).
%   It fetches the decimated data of the upload DAQ from the server and prints it on USB:
%       PT sample           "<volts>"                    one line per sample
%       load cell sample    "L:<raw count>"
%       calibration         "CALRX:OK rev=.. pt_set=.. pt_a=.. pt_b=.. lc_set=.. lc_a=.. lc_b=.."
%       end of one batch    "T:DTPT <us>", "T:DTLC <us>", "T:LINK <drops> <rssi_upload> <rssi_own>", "T:AGE <ms>"
%   The T:AGE line is the end of a batch. The samples before it get their time here: the last one at the time of the
%   line, the others one sample interval (DTPT or DTLC) earlier each. A gap in the data stays a gap in the plot.
%
%   "Data age" is the age of the newest data on the server when the DAQ asked for it. The real delay is a little more
%   (network and the fetch). The banner turns red when the data age is above the limit, or when no batch comes at all.
%
%   The pressure shows in PSI and the force in N when the upload DAQ has a calibration (A x + B, saved in that DAQ).
%   Without it: volts and mV/V.

    % --- constants ---
    lcMvVPerCount = 1000 / 2^31;      % load cell: counts / (gain 128 * 2^24) as mV/V (the same as DAQ_Calibration_App)
    keepS = 660;                      % seconds of data that stay in memory (a bit more than the largest window)
    noBatchS = 8;                     % no batch for this long = no link to the server (the server answers every 5 s at most)
    banner0 = 'Not connected';

    % --- state ---
    s = []; tmr = [];
    carry = '';
    t0 = tic;
    ptT = []; ptV = []; lcT = []; lcRaw = [];      % time (s since start), volts, counts
    pendPT = []; pendLC = [];                      % samples of the batch that is not finished (no T:AGE line yet)
    dtPT = 0; dtLC = 0;                            % sample intervals of the last batch, in s (0 = not known)
    ageMs = NaN; lastBatchT = -Inf; drops = NaN; rssiUp = NaN; rssiOwn = NaN; nBatches = 0;
    cal = struct('valid', false, 'rev', 0, 'ptSet', false, 'ptA', 1, 'ptB', 0, 'lcSet', false, 'lcA', 1, 'lcB', 0);
    daqMode = ''; daqRun = ''; daqLink = NaN;
    tick = 0;
    logFile = -1;
    portName = '';

    if isdeployed
        homeDir = getenv('USERPROFILE');
        if isempty(homeDir), homeDir = getenv('HOME'); end
        outDir = fullfile(homeDir, 'Documents', 'DAQ Calibration');
    else
        outDir = fullfile(pwd, 'DAQ Calibration');
    end
    if ~exist(outDir, 'dir'), mkdir(outDir); end

    % --- UI ---
    fig = uifigure('Name', 'DAQ Live Monitor (telemetry)', 'Position', [80, 60, 1150, 760]);
    statusLbl = uilabel(fig, 'Position', [20, 725, 380, 22], 'Text', 'Status: Disconnected', 'FontWeight', 'bold');
    uibutton(fig, 'Position', [410, 725, 100, 22], 'Text', 'Auto-Connect', 'ButtonPushedFcn', @connectDAQ);
    uilabel(fig, 'Position', [530, 725, 90, 22], 'Text', 'Window:');
    winDrop = uidropdown(fig, 'Position', [595, 725, 90, 22], 'Items', {'30 s', '2 min', '10 min'}, 'Value', '2 min');
    uilabel(fig, 'Position', [705, 725, 130, 22], 'Text', 'Stale after (s):');
    staleField = uieditfield(fig, 'numeric', 'Position', [800, 725, 50, 22], 'Value', 3, 'Limits', [0.5 600]);
    logCheck = uicheckbox(fig, 'Position', [870, 725, 150, 22], 'Text', 'Log to CSV', 'Value', true);

    bannerLbl = uilabel(fig, 'Position', [20, 670, 1110, 45], 'Text', banner0, 'FontSize', 24, 'FontWeight', 'bold', ...
        'HorizontalAlignment', 'center', 'BackgroundColor', [0.55 0.55 0.55], 'FontColor', [1 1 1]);

    ptBig = uilabel(fig, 'Position', [20, 615, 540, 45], 'Text', 'PT: --', 'FontSize', 30, 'FontWeight', 'bold');
    lcBig = uilabel(fig, 'Position', [590, 615, 540, 45], 'Text', 'Load cell: --', 'FontSize', 30, 'FontWeight', 'bold');
    infoLbl = uilabel(fig, 'Position', [20, 580, 1110, 30], 'Text', '', 'FontSize', 13);

    axP = uiaxes(fig, 'Position', [20, 300, 1110, 270]);
    grid(axP, 'on'); hold(axP, 'on'); ylabel(axP, 'Pressure (PSI)'); title(axP, 'PT');
    linePT = plot(axP, NaN, NaN, 'b-', 'LineWidth', 1.2);
    axL = uiaxes(fig, 'Position', [20, 20, 1110, 270]);
    grid(axL, 'on'); hold(axL, 'on'); ylabel(axL, 'Force (N)'); xlabel(axL, 'Time (s since start of the monitor)'); title(axL, 'Load cell');
    lineLC = plot(axL, NaN, NaN, 'r-', 'LineWidth', 1.2);

    fig.CloseRequestFcn = @(~, ~) closeApp();

    % =====================================================================
    function connectDAQ(~, ~)
        stopTimer();
        closePort();
        statusLbl.Text = 'Status: Scanning...';
        drawnow;
        ports = serialportlist("available");
        for i = 1:length(ports)
            tmp = [];
            try
                tmp = serialport(ports(i), 115200, 'Timeout', 3);
                write(tmp, "MATLAB PING", "string");
                pause(1.5);
                if tmp.NumBytesAvailable > 0
                    resp = read(tmp, tmp.NumBytesAvailable, "char");
                    if contains(resp, "01010101 10101010 DAQBOARD V1 01010101 10101010")
                        s = tmp;
                        flush(s);
                        carry = '';
                        portName = char(ports(i));
                        statusLbl.Text = ['Status: Connected to ', portName];
                        openLog();
                        tick = 0;
                        tmr = timer('ExecutionMode', 'fixedSpacing', 'Period', 0.1, 'TimerFcn', @onTick);
                        start(tmr);
                        return;
                    end
                end
                delete(tmp);
            catch
                if ~isempty(tmp) && isvalid(tmp), delete(tmp); end
            end
        end
        statusLbl.Text = 'Status: DAQ Not Found';
    end

    function openLog()
        if logFile ~= -1, fclose(logFile); logFile = -1; end
        if ~logCheck.Value, return; end
        fn = fullfile(outDir, sprintf('telemetry_live_%s.csv', char(datetime('now', 'Format', 'yyyy-MM-dd_HH-mm-ss'))));
        logFile = fopen(fn, 'a');
        if logFile ~= -1
            fprintf(logFile, 'wall_time,sensor,time_s,raw,value,unit\n');
        end
    end

    function onTick(~, ~)
        try
            tick = tick + 1;
            if tick == 1 || mod(tick, 50) == 0       % the mode of the DAQ and the state of its own WiFi, at the start and every 5 s
                writeline(s, "WIFI GET");
            end
            readBatch();
            updateScreen();
        catch ME
            if isempty(s) || ~isvalid(s) || contains(ME.identifier, 'ReadError') || contains(lower(ME.message), 'invalid')
                statusLbl.Text = 'Status: Disconnected (connection to the DAQ lost)';
                stopTimer();
                closePort();
            end
        end
    end

    % One read of the serial port: every complete line is one of the line types of the header of this file.
    function readBatch()
        n = s.NumBytesAvailable;
        if n == 0, return; end
        str = [carry, char(read(s, n, "char"))];
        lastLF = find(str == newline, 1, 'last');
        if isempty(lastLF)
            carry = str;
            return;
        end
        carry = str(lastLF+1:end);
        lines = splitlines(str(1:lastLF));
        for k = 1:numel(lines)
            ln = strtrim(lines{k});
            if isempty(ln), continue; end
            c1 = ln(1);
            if (c1 >= '0' && c1 <= '9') || c1 == '-' || c1 == '+' || c1 == '.'
                v = str2double(ln);
                if ~isnan(v), pendPT(end+1) = v; end                     %#ok<AGROW>
            elseif startsWith(ln, 'L:')
                v = str2double(ln(3:end));
                if ~isnan(v), pendLC(end+1) = v; end                     %#ok<AGROW>
            elseif startsWith(ln, 'T:DTPT')
                dtPT = str2double(extractAfter(ln, 'T:DTPT')) * 1e-6;
            elseif startsWith(ln, 'T:DTLC')
                dtLC = str2double(extractAfter(ln, 'T:DTLC')) * 1e-6;
            elseif startsWith(ln, 'T:LINK')
                v = sscanf(extractAfter(ln, 'T:LINK'), '%f');
                if numel(v) >= 3, drops = v(1); rssiUp = v(2); rssiOwn = v(3); end
            elseif startsWith(ln, 'T:AGE')
                ageMs = str2double(extractAfter(ln, 'T:AGE'));
                if ageMs < 0, ageMs = NaN; end                           % -1 = the server has no data yet
                commitBatch(toc(t0));
            elseif startsWith(ln, 'CALRX:OK')
                applyCal(ln);
            elseif startsWith(ln, 'WIFI:OK')
                applyWifi(ln);
            end
        end
    end

    function commitBatch(tNow)
        nBatches = nBatches + 1;
        lastBatchT = tNow;
        wall = char(datetime('now', 'Format', 'yyyy-MM-dd HH:mm:ss.SSS'));
        if ~isempty(pendPT)
            m = numel(pendPT);
            dt = dtPT; if dt <= 0, dt = 0.5 / m; end
            t = tNow - (m-1:-1:0) * dt;
            ptT = [ptT, t]; ptV = [ptV, pendPT];                         %#ok<AGROW>
            logSamples(wall, 'PT', t, pendPT, ptValue(pendPT), ptUnit());
        end
        if ~isempty(pendLC)
            m = numel(pendLC);
            dt = dtLC; if dt <= 0, dt = 0.5 / m; end
            t = tNow - (m-1:-1:0) * dt;
            lcT = [lcT, t]; lcRaw = [lcRaw, pendLC];                     %#ok<AGROW>
            logSamples(wall, 'LC', t, pendLC, lcValue(pendLC), lcUnit());
        end
        pendPT = []; pendLC = [];
        keepPT = ptT >= tNow - keepS; ptT = ptT(keepPT); ptV = ptV(keepPT);
        keepLC = lcT >= tNow - keepS; lcT = lcT(keepLC); lcRaw = lcRaw(keepLC);
    end

    function logSamples(wall, sensor, t, raw, val, unit)
        if logFile == -1, return; end
        for j = 1:numel(t)
            fprintf(logFile, '%s,%s,%.4f,%.9g,%.9g,%s\n', wall, sensor, t(j), raw(j), val(j), unit);
        end
    end

    function applyCal(ln)
        tok = regexp(ln, ['^CALRX:OK\s+rev=(\d+)\s+pt_set=(\d)\s+pt_a=(\S+)\s+pt_b=(\S+)', ...
            '\s+lc_set=(\d)\s+lc_a=(\S+)\s+lc_b=(\S+)'], 'tokens', 'once');
        if isempty(tok), return; end
        v = str2double(tok);
        cal.valid = true; cal.rev = v(1);
        cal.ptSet = v(2) ~= 0; cal.ptA = v(3); cal.ptB = v(4);
        cal.lcSet = v(5) ~= 0; cal.lcA = v(6); cal.lcB = v(7);
        if ~all(isfinite(v(3:4))), cal.ptSet = false; end
        if ~all(isfinite(v(6:7))), cal.lcSet = false; end
        ylabel(axP, ['Pressure (', ptUnit(), ')']);
        ylabel(axL, [lcName(), ' (', lcUnit(), ')']);
    end

    function applyWifi(ln)
        tok = regexp(ln, 'mode=(\w+)\s+run=(\w+).*?link=(\d)', 'tokens', 'once');
        if isempty(tok), return; end
        daqMode = tok{1}; daqRun = tok{2}; daqLink = str2double(tok{3});
    end

    % --- conversion of the readings ---
    function val = ptValue(volts)
        if cal.ptSet, val = cal.ptA * volts + cal.ptB; else, val = volts; end
    end
    function u = ptUnit()
        if cal.ptSet, u = 'PSI'; else, u = 'V'; end
    end
    function val = lcValue(counts)
        mv = counts * lcMvVPerCount;
        if cal.lcSet, val = cal.lcA * mv + cal.lcB; else, val = mv; end
    end
    function u = lcUnit()
        if cal.lcSet, u = 'N'; else, u = 'mV/V'; end
    end
    function nm = lcName()
        if cal.lcSet, nm = 'Force'; else, nm = 'Output'; end
    end

    function updateScreen()
        tNow = toc(t0);
        switch winDrop.Value
            case '30 s', win = 30;
            case '10 min', win = 600;
            otherwise, win = 120;
        end
        % plots
        if ~isempty(ptT)
            sel = ptT >= tNow - win;
            set(linePT, 'XData', ptT(sel), 'YData', ptValue(ptV(sel)));
            ptBig.Text = sprintf('PT: %.2f %s', ptValue(ptV(end)), ptUnit());
        end
        if ~isempty(lcT)
            sel = lcT >= tNow - win;
            set(lineLC, 'XData', lcT(sel), 'YData', lcValue(lcRaw(sel)));
            lcBig.Text = sprintf('%s: %.2f %s', lcName(), lcValue(lcRaw(end)), lcUnit());
        end
        xlim(axP, [tNow - win, tNow]);
        xlim(axL, [tNow - win, tNow]);

        % link state and banner
        sinceBatch = tNow - lastBatchT;
        limitS = staleField.Value;
        if ~isempty(daqRun) && ~strcmp(daqRun, 'DOWNLOAD')
            setBanner(sprintf('This DAQ is not in DOWNLOAD mode (run=%s). The data is not from the rocket.', daqRun), [0.85 0.45 0.0]);
        elseif nBatches == 0
            setBanner('Waiting for the first batch from the server...', [0.55 0.55 0.55]);
        elseif sinceBatch > noBatchS
            setBanner(sprintf('NO LINK to the server: no data for %.0f s', sinceBatch), [0.80 0.0 0.0]);
        elseif isnan(ageMs)
            setBanner('Server has no data from the rocket yet', [0.80 0.0 0.0]);
        elseif ageMs / 1000 > limitS
            setBanner(sprintf('STALE: the newest data is %.1f s old', ageMs / 1000), [0.80 0.0 0.0]);
        else
            setBanner(sprintf('LIVE   data age %.1f s', ageMs / 1000), [0.0 0.55 0.15]);
        end
        infoLbl.Text = sprintf(['Upload DAQ: drops %s, RSSI %s dBm   |   This DAQ: RSSI %s dBm, link to server %s   |   ', ...
            'batches %d   |   calibration %s'], num2strOrDash(drops), num2strOrDash(rssiUp), num2strOrDash(rssiOwn), ...
            linkText(), nBatches, calText());
    end

    function setBanner(txt, color)
        bannerLbl.Text = txt;
        bannerLbl.BackgroundColor = color;
    end

    function t = num2strOrDash(v)
        if isnan(v), t = '--'; else, t = sprintf('%g', v); end
    end

    function t = linkText()
        if isnan(daqLink), t = '--'; elseif daqLink ~= 0, t = 'up'; else, t = 'DOWN'; end
    end

    function t = calText()
        if ~cal.valid, t = 'not received yet (volts and mV/V)'; else, t = sprintf('rev %d', cal.rev); end
    end

    % =====================================================================
    function stopTimer()
        if ~isempty(tmr) && isvalid(tmr)
            stop(tmr);
            delete(tmr);
        end
        tmr = [];
    end

    function closePort()
        if ~isempty(s) && isvalid(s), delete(s); end
        s = [];
        if logFile ~= -1, fclose(logFile); logFile = -1; end
    end

    function closeApp()
        stopTimer();
        closePort();
        delete(fig);
    end

    if isdeployed
        waitfor(fig);
    end
end
