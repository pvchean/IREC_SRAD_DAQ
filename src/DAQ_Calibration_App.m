function DAQ_Calibration_App()
    % --- Shared Variables ---
    s = []; % Serial port object
    daqTimer = []; % Timer for batch-draining serial buffer without 860Hz callback lag
    serialCarryover = ''; % Holds incomplete line fragments between batch reads

    decimationFactor = 64;
    sampleCount = 0;

    liveV = []; % Live voltage points (decimated)
    liveT = []; % Timestamps
    windowSize = 100; % Default slider value (kept at 100 for fast uifigure DOM rendering)

    calV = []; % Saved manual calibration voltages
    calP = []; % Saved manual calibration pressures

    currentA = 1; % Default linear fit slope
    currentB = 0; % Default linear fit offset

    isLiveLogging = true; % Flag controlling whether to append to live CSV

    % --- DAQ chain and file transfer state ---
    chainTimer = [];   % Timer that asks the DAQ for the boards in the UART chain (DAQS)
    selfId = -1;       % Device ID of the DAQ on USB (-1 = unknown)
    seenIds = [];      % Device IDs of the other DAQs in the chain
    portName = '';     % Serial port of the connected DAQ
    xferBusy = false;  % True while a FILES/GET transfer owns the serial port

    % Files & Plot window handles and data
    filesFig = []; boardList = []; localList = []; dirField = []; filesInfoLbl = [];
    axF = []; startSlider = []; endSlider = []; startLbl = []; endLbl = []; statsLbl = []; unitDrop = [];
    boardFiles = struct('src', {}, 'name', {}, 'bytes', {});
    plotT = []; plotRaw = []; plotSeq = []; plotName = '';

    % Handles for performant live plotting and windows
    lineV = [];
    lineP = [];
    calFig = [];           % Handle for the calibration window
    calFitLine = [];       % Handle for the dashed fit line in cal window
    calTransientLine = []; % Live transient point on the calibration curve
    calTxt = [];           % Handle for the error text area in cal window

    % --- Output Directory & File Naming ---
    % In MATLAB: next to the current folder. As a compiled app: in Documents (the install folder is read-only).
    if isdeployed
        homeDir = getenv('USERPROFILE');               % Windows
        if isempty(homeDir), homeDir = getenv('HOME'); end
        outDir = fullfile(homeDir, 'Documents', 'DAQ Calibration');
    else
        outDir = fullfile(pwd, 'DAQ Calibration');
    end
    if ~exist(outDir, 'dir')
        mkdir(outDir);
    end

    timestampStr = char(datetime('now', 'Format', 'yyyy-MM-dd_HH-mm-ss'));

    liveFile = fullfile(outDir, sprintf('live_data_%s.csv', timestampStr));
    liveDataFile = fopen(liveFile, 'a');
    fprintf(liveDataFile, 'Time,Voltage\n');

    calFile = fullfile(outDir, sprintf('manual_calibration_points_%s.csv', timestampStr));
    calDataFile = fopen(calFile, 'a');
    fprintf(calDataFile, 'Voltage,Pressure\n');

    % --- Main UI Layout ---
    fig = uifigure('Name', 'DAQ Calibration Tool', 'Position', [100, 100, 1100, 710]);

    % DAQ chain row: the boards on the UART chain and the one on USB
    chainLbl = uilabel(fig, 'Position', [20, 675, 900, 22], 'Text', 'DAQs in chain: (not connected)', 'FontWeight', 'bold');
    btnFiles = uibutton(fig, 'Position', [940, 675, 140, 22], 'Text', 'Files & Plot...', 'ButtonPushedFcn', @openFilesWindow);
    downloadDir = fullfile(outDir, 'Downloads');

    % Status, Connection, and Window Size Controls
    statusLbl = uilabel(fig, 'Position', [20, 640, 340, 22], 'Text', 'Status: Disconnected', 'FontWeight', 'bold');
    btnConnect = uibutton(fig, 'Position', [370, 640, 100, 22], 'Text', 'Auto-Connect', 'ButtonPushedFcn', @connectDAQ);

    uilabel(fig, 'Position', [510, 640, 130, 22], 'Text', 'Live Samples Shown:');
    windowSlider = uislider(fig, 'Position', [650, 650, 180, 3], 'Limits', [10, 500], 'Value', windowSize, 'ValueChangedFcn', @updateWindowSize);

    % --- Section 1: Old Calibration Buttons ---
    pnlCal = uipanel(fig, 'Title', 'PT Calibration (Point-by-Point)', 'FontWeight', 'bold', 'Position', [20, 560, 1060, 65]);

    uilabel(pnlCal, 'Position', [15, 12, 210, 22], 'Text', 'Regulator Pressure Setpoint (PSI):');
    numInput = uieditfield(pnlCal, 'numeric', 'Position', [230, 12, 70, 22], 'Value', 0);
    btnCapture = uibutton(pnlCal, 'Position', [315, 12, 160, 22], 'Text', 'Capture Average & Save', 'ButtonPushedFcn', @captureCal);
    btnViewCal = uibutton(pnlCal, 'Position', [490, 12, 150, 22], 'Text', 'Calculate fit & View', 'ButtonPushedFcn', @openCalWindow);

    % --- Section 2: Manual Ax+B Formula & Zero Pressure ---
    pnlParams = uipanel(fig, 'Title', 'Manual Formula (Ax + B) & Zeroing', 'FontWeight', 'bold', 'Position', [20, 485, 1060, 65]);

    uilabel(pnlParams, 'Position', [15, 12, 110, 22], 'Text', 'Pressure (PSI) =');
    inputA = uieditfield(pnlParams, 'numeric', 'Position', [125, 12, 85, 22], ...
        'Value', currentA, 'ValueDisplayFormat', '%.5f', 'ValueChangedFcn', @updateNumericAB);

    uilabel(pnlParams, 'Position', [218, 12, 40, 22], 'Text', '* V +');
    inputB = uieditfield(pnlParams, 'numeric', 'Position', [260, 12, 85, 22], ...
        'Value', currentB, 'ValueDisplayFormat', '%.5f', 'ValueChangedFcn', @updateNumericAB);

    btnZeroAtm = uibutton(pnlParams, 'Position', [365, 12, 190, 22], ...
        'Text', 'Zero Out Pressure (Tare)', 'ButtonPushedFcn', @zeroPressure);

    tareStatusLbl = uilabel(pnlParams, 'Position', [570, 12, 450, 22], 'Text', '');

    % --- Plots ---
    % Plot 1: Live Voltage
    ax1 = uiaxes(fig, 'Position', [20, 20, 510, 440]);
    title(ax1, 'Live Voltage (Transducer)');
    xlabel(ax1, 'Samples');
    ylabel(ax1, 'Voltage (V)');
    grid(ax1, 'on');
    ylim(ax1, [0, 5.5]);
    hold(ax1, 'on');
    lineV = plot(ax1, NaN, NaN, 'b--', 'LineWidth', 1.5);

    % Plot 2: Live Estimated Pressure
    ax2 = uiaxes(fig, 'Position', [570, 20, 510, 440]);
    title(ax2, 'Live Estimated Pressure (PSI)');
    xlabel(ax2, 'Samples');
    ylabel(ax2, 'Pressure (PSI)');
    grid(ax2, 'on');
    hold(ax2, 'on');
    lineP = plot(ax2, NaN, NaN, 'r--', 'LineWidth', 1.5);

    % --- Fast Helper Functions (Separated from Hot Loop) ---
    function updatePlotsOnly()
        % Lightweight plot refresh (Safe for high-frequency calls)
        if isempty(liveV)
            return;
        end
        liveEstP = currentA .* liveV + currentB;

        if isvalid(lineV) && isvalid(lineP)
            lineV.XData = liveT;
            lineV.YData = liveV;

            lineP.XData = liveT;
            lineP.YData = liveEstP;
        end

        if ~isempty(calTransientLine) && isvalid(calTransientLine)
            calTransientLine.XData = liveV(end);
            calTransientLine.YData = liveEstP(end);
        end
        drawnow limitrate;
    end

    function updateCalWindowDetails()
        % Only called on manual button/field actions, NEVER in the 860Hz serial stream
        if ~isempty(calFig) && isvalid(calFig)
            if ~isempty(calFitLine) && isvalid(calFitLine)
                calFitLine.YData = currentA .* calFitLine.XData + currentB;
            end
            if ~isempty(calTxt) && isvalid(calTxt) && ~isempty(calV)
                est_calP = currentA .* calV + currentB;
                errs = calP - est_calP;
                sq_errs = errs.^2;

                lines = { ...
                    '--- CALIBRATION FORMULA ---', ...
                    sprintf('P = %.5f * Volts %+.5f\n', currentA, currentB), ...
                    '--- Non-Linearity Errors ---' ...
                };
                for idx = 1:length(calV)
                    lines{end+1} = sprintf('Pt %d (%.2f PSI Target):', idx, calP(idx));
                    lines{end+1} = sprintf('  Error:    %+.4f PSI', errs(idx));
                    lines{end+1} = sprintf('  Sq Error:  %.4e PSI^2', sq_errs(idx));
                end
                calTxt.Value = lines;
            end
        end
    end

    % --- Callbacks & Core Logic ---
    function updateNumericAB(~, ~)
        currentA = inputA.Value;
        currentB = inputB.Value;
        updatePlotsOnly();
        updateCalWindowDetails();
    end

    function zeroPressure(~, ~)
        if isempty(liveV)
            uialert(fig, 'No live voltage data available to zero the offset.', 'Error');
            return;
        end

        % Calculate steady-state average voltage from last 10 decimated points
        ptsToAvg = min(10, length(liveV));
        avgV = mean(liveV(end-ptsToAvg+1:end));

        % Solve for B such that P = A * avgV + B = 0 PSI
        currentB = -currentA * avgV;
        inputB.Value = currentB;

        % Update non-blocking status label instead of stalling serial queue with uialert
        tareStatusLbl.Text = sprintf('Zeroed at %.4f V (New B = %+.5f PSI)', avgV, currentB);

        updatePlotsOnly();
        updateCalWindowDetails();
    end

    function connectDAQ(~, ~)
        % Clean up any prior connection
        if ~isempty(daqTimer) && isvalid(daqTimer)
            stop(daqTimer);
            delete(daqTimer);
        end
        if ~isempty(chainTimer) && isvalid(chainTimer)
            stop(chainTimer);
            delete(chainTimer);
        end
        if ~isempty(s) && isvalid(s)
            clear s;
        end
        selfId = -1;
        seenIds = [];
        updateChainLabel();

        statusLbl.Text = 'Status: Scanning...';
        drawnow;
        ports = serialportlist("available");

        for i = 1:length(ports)
            try
                temp_s = serialport(ports(i), 115200, 'Timeout', 3);
                write(temp_s, "MATLAB PING", "string");
                pause(1.5);

                if temp_s.NumBytesAvailable > 0
                    resp = read(temp_s, temp_s.NumBytesAvailable, "char");
                    if contains(resp, "01010101 10101010 DAQBOARD V1 01010101 10101010")
                        s = temp_s;
                        flush(s); % Clear handshake leftovers
                        serialCarryover = '';
                        portName = char(ports(i));
                        statusLbl.Text = ['Status: Connected to ', portName];

                        % Use a 20 Hz batch-drain timer instead of 860 Hz single-line callbacks
                        daqTimer = timer('ExecutionMode', 'fixedSpacing', ...
                                         'Period', 0.05, ...
                                         'TimerFcn', @readSerialBatch);
                        start(daqTimer);

                        % Ask for the chain every 2 s; the DAQS reply comes between the number lines
                        chainTimer = timer('ExecutionMode', 'fixedSpacing', ...
                                           'Period', 2, ...
                                           'TimerFcn', @requestDaqs);
                        start(chainTimer);
                        return;
                    end
                end
                clear temp_s;
            catch
                % Ignore and inspect next port
            end
        end
        statusLbl.Text = 'Status: DAQ Not Found';
    end

    function readSerialBatch(~, ~)
        try
            if isempty(s) || ~isvalid(s)
                return;
            end

            bytesAvail = s.NumBytesAvailable;
            if bytesAvail == 0
                return;
            end

            % Read entire OS serial buffer in a single C-level call
            rawChunk = read(s, bytesAvail, "char");
            fullStr = [serialCarryover, char(rawChunk)];

            % Find the last newline so we only parse complete lines
            lastLF = find(fullStr == newline, 1, 'last');
            if isempty(lastLF)
                serialCarryover = fullStr;
                return;
            end

            completeLines = fullStr(1:lastLF);
            serialCarryover = fullStr(lastLF+1:end);

            % Reply lines (they start with a letter, e.g. DAQS:...) come between the number lines
            txtLines = regexp(completeLines, '^[A-Za-z][^\r\n]*', 'match', 'lineanchors');
            if ~isempty(txtLines)
                for k = 1:numel(txtLines)
                    handleReply(strtrim(txtLines{k}));
                end
                completeLines = regexprep(completeLines, '^[A-Za-z][^\r\n]*', '', 'lineanchors');
            end

            % Vectorized C-speed float parsing of all samples in this chunk
            vals = sscanf(completeLines, '%f');
            if isempty(vals)
                return;
            end

            % Clamp voltage strictly between 0 and 5.5V
            vals = max(0, min(5.5, vals));

            % Find exact indices that hit the decimation factor (64)
            numNew = length(vals);
            sampleIndices = (sampleCount + 1) : (sampleCount + numNew);
            sampleCount = sampleCount + numNew;

            decMask = mod(sampleIndices, decimationFactor) == 0;
            if any(decMask)
                decV = vals(decMask)';
                decT = sampleIndices(decMask) ./ decimationFactor;

                liveV = [liveV, decV];
                liveT = [liveT, decT];

                % Log decimated points to CSV if active
                if isLiveLogging && liveDataFile ~= -1
                    t_stamp = char(datetime('now', 'Format', 'yyyy-MM-dd HH:mm:ss.SSS'));
                    for k = 1:length(decV)
                        fprintf(liveDataFile, '%s,%f\n', t_stamp, decV(k));
                    end
                end

                % Enforce moving window
                if length(liveV) > windowSize
                    liveV = liveV(end-windowSize+1:end);
                    liveT = liveT(end-windowSize+1:end);
                end

                % Perform single UI update for the entire batch
                updatePlotsOnly();
            end
        catch ME
            if isempty(s) || ~isvalid(s) || contains(ME.identifier, 'ReadError') || contains(lower(ME.message), 'invalid')
                statusLbl.Text = 'Status: Disconnected (Connection Lost)';
                if ~isempty(daqTimer) && isvalid(daqTimer)
                    stop(daqTimer);
                end
                if ~isempty(chainTimer) && isvalid(chainTimer)
                    stop(chainTimer);
                end
                s = [];
                selfId = -1;
                seenIds = [];
                updateChainLabel();
            end
        end
    end

    function updateWindowSize(src, ~)
        windowSize = round(src.Value);
        if length(liveV) > windowSize
            liveV = liveV(end-windowSize+1:end);
            liveT = liveT(end-windowSize+1:end);
            updatePlotsOnly();
        end
    end

    function captureCal(~, ~)
        if isempty(liveV)
            uialert(fig, 'No live data available yet.', 'Error');
            return;
        end

        ptsToAvg = min(10, length(liveV));
        avgV = mean(liveV(end-ptsToAvg+1:end));
        pSet = numInput.Value;

        calV(end+1) = avgV;
        calP(end+1) = pSet;

        if calDataFile ~= -1
            fprintf(calDataFile, '%f,%f\n', avgV, pSet);
        end

        if length(calV) >= 2
            coeffs = polyfit(calV, calP, 1);
            currentA = coeffs(1);
            currentB = coeffs(2);
            inputA.Value = currentA;
            inputB.Value = currentB;
            updatePlotsOnly();
            updateCalWindowDetails();
        end

        uialert(fig, sprintf('Captured %.3f V at %.2f PSI.', avgV, pSet), 'Saved', 'Icon', 'success');
    end

    function openCalWindow(~, ~)
        if length(calV) < 2
            uialert(fig, 'Need at least 2 manual points to least-squares fit a line.', 'Not Enough Data');
            return;
        end

        if ~isempty(calFig) && isvalid(calFig)
            delete(calFig);
        end

        if isLiveLogging
            isLiveLogging = false;
            if liveDataFile ~= -1
                fclose(liveDataFile);
                liveDataFile = -1;
            end
            if contains(statusLbl.Text, 'Connected')
                statusLbl.Text = [statusLbl.Text, ' (Live Logging Stopped)'];
            end
        end

        % Recalculate least-squares fit from saved calibration points
        coeffs = polyfit(calV, calP, 1);
        currentA = coeffs(1);
        currentB = coeffs(2);
        inputA.Value = currentA;
        inputB.Value = currentB;

        % Create Calibration Window
        calFig = uifigure('Name', 'Calibration Formula & Results', 'Position', [200, 200, 900, 500]);
        calTxt = uitextarea(calFig, 'Position', [20, 20, 300, 460], 'Editable', 'off');

        axCal = uiaxes(calFig, 'Position', [340, 20, 540, 460]);
        hold(axCal, 'on');

        v_min = max(0, min([calV, liveV]) - 0.5);
        v_max = min(5.5, max([calV, liveV]) + 0.5);

        v_range = linspace(v_min, v_max, 100);
        p_line = currentA .* v_range + currentB;
        calFitLine = plot(axCal, v_range, p_line, 'k--', 'LineWidth', 1.5, 'DisplayName', 'ax+b Fit');

        plot(axCal, calV, calP, '^', 'MarkerFaceColor', 'b', 'MarkerEdgeColor', 'b', ...
            'MarkerSize', 8, 'DisplayName', 'Manual Pts');

        live_estP_current = currentA .* liveV(end) + currentB;
        calTransientLine = plot(axCal, liveV(end), live_estP_current, 'ko', ...
            'MarkerSize', 6, 'DisplayName', 'Live Transient');

        title(axCal, 'Calibration Linearity Check');
        xlabel(axCal, 'Transducer Voltage (Volts)');
        ylabel(axCal, 'Estimated Pressure (PSI)');
        legend(axCal, 'Location', 'best');
        grid(axCal, 'on');
        hold(axCal, 'off');

        updatePlotsOnly();
        updateCalWindowDetails();
    end

    % =====================================================================
    % --- DAQ chain (DAQS command) ---
    % Firmware reply: "DAQS:SELF=<id> SEEN=<id>,<id>,..." (SEEN can be empty)
    % =====================================================================
    function requestDaqs(~, ~)
        if xferBusy || isempty(s) || ~isvalid(s)
            return;
        end
        try
            writeline(s, "DAQS");
        catch
        end
    end

    function handleReply(t)
        tok = regexp(t, 'DAQS:SELF=(\d+) SEEN=([\d,]*)', 'tokens', 'once');
        if ~isempty(tok)
            selfId = str2double(tok{1});
            ids = str2double(strsplit(tok{2}, ','));
            seenIds = ids(~isnan(ids));
            updateChainLabel();
        end
    end

    function updateChainLabel()
        if selfId < 0
            if isempty(s)
                chainLbl.Text = 'DAQs in chain: (not connected)';
            else
                chainLbl.Text = 'DAQs in chain: (waiting for the DAQ)';
            end
            if ~isempty(filesInfoLbl) && isvalid(filesInfoLbl)
                filesInfoLbl.Text = 'Connected DAQ: none';
            end
            return;
        end
        allIds = unique([seenIds, selfId]);
        names = strings(1, numel(allIds));
        for k = 1:numel(allIds)
            if allIds(k) == selfId
                names(k) = sprintf('[DAQ%d on USB]', allIds(k));
            else
                names(k) = sprintf('DAQ%d', allIds(k));
            end
        end
        chainLbl.Text = ['DAQs in chain: ', char(strjoin(names, '   '))];
        statusLbl.Text = sprintf('Status: Connected to %s (DAQ%d)', portName, selfId);
        if ~isLiveLogging
            statusLbl.Text = [statusLbl.Text, ' (Live Logging Stopped)'];
        end
        if ~isempty(filesInfoLbl) && isvalid(filesInfoLbl)
            filesInfoLbl.Text = sprintf('Connected DAQ: DAQ%d   Chain: %s', selfId, char(strjoin(names, ' ')));
        end
    end

    % =====================================================================
    % --- Serial transfer helpers (FILES and GET commands) ---
    % =====================================================================
    function pauseSerial()
        xferBusy = true;
        if ~isempty(daqTimer) && isvalid(daqTimer), stop(daqTimer); end
        if ~isempty(chainTimer) && isvalid(chainTimer), stop(chainTimer); end
        flush(s);
        serialCarryover = '';
    end

    function resumeSerial()
        xferBusy = false;
        if isempty(s) || ~isvalid(s)
            return;
        end
        serialCarryover = '';
        if ~isempty(daqTimer) && isvalid(daqTimer), start(daqTimer); end
        if ~isempty(chainTimer) && isvalid(chainTimer), start(chainTimer); end
    end

    function t = waitLine(prefixes, timeoutS)
        % Read lines until one starts with one of the prefixes. Live number lines are skipped.
        t0 = tic;
        while toc(t0) < timeoutS
            ln = [];
            try
                if s.NumBytesAvailable > 0
                    ln = readline(s);
                else
                    pause(0.01);
                end
            catch
            end
            if ~isempty(ln)
                ln = strtrim(char(ln));
                for k = 1:numel(prefixes)
                    if startsWith(ln, prefixes{k})
                        t = ln;
                        return;
                    end
                end
            end
        end
        error('DAQ:timeout', 'No reply %s from the DAQ.', strjoin(prefixes, ' / '));
    end

    function files = queryFiles()
        % FILES reply: "FILE PFS <id> <bytes> <flags>" and "FILE SD <name> <bytes>", then "FILES:END"
        files = struct('src', {}, 'name', {}, 'bytes', {});
        pauseSerial();
        cleanup = onCleanup(@resumeSerial);
        writeline(s, "FILES");
        waitLine({'FILES:BEGIN'}, 5);
        while true
            ln = waitLine({'FILE ', 'FILES:END'}, 5);
            if strcmp(ln, 'FILES:END')
                break;
            end
            p = strsplit(ln);
            if numel(p) >= 4
                files(end+1) = struct('src', p{2}, 'name', p{3}, 'bytes', str2double(p{4})); %#ok<AGROW>
            end
        end
    end

    function [data, ok, msg] = getFile(src, name, dlg)
        % GET reply: "GET:BEGIN ...", frames [u16 len LE][len bytes], a 0 frame, "GET:END bytes=<n> crc32=<hex> OK|ERR"
        pauseSerial();
        cleanup = onCleanup(@resumeSerial);
        writeline(s, sprintf('GET %s %s', src, name));
        hdr = waitLine({'GET:BEGIN', 'GET:ERR'}, 10);
        if startsWith(hdr, 'GET:ERR')
            data = uint8([]); ok = false; msg = hdr;
            return;
        end
        p = strsplit(hdr);
        approx = max(str2double(p{end}), 1);
        buf = zeros(approx + 65536, 1, 'uint8');
        n = 0;
        frames = 0;
        while true
            h = read(s, 2, 'uint8');
            if numel(h) < 2
                error('DAQ:timeout', 'The DAQ stopped sending (frame header).');
            end
            len = double(h(1)) + 256 * double(h(2));
            if len == 0
                break;
            end
            d = read(s, len, 'uint8');
            if numel(d) < len
                error('DAQ:timeout', 'The DAQ stopped sending (frame data).');
            end
            if n + len > numel(buf)
                buf(2 * numel(buf)) = 0; % grow
            end
            buf(n+1:n+len) = d(:);
            n = n + len;
            frames = frames + 1;
            if ~isempty(dlg) && mod(frames, 16) == 0
                dlg.Value = min(1, n / approx);
            end
        end
        data = buf(1:n);
        tail = waitLine({'GET:END'}, 5);
        tok = regexp(tail, 'bytes=(\d+) crc32=([0-9A-Fa-f]{8}) (\w+)', 'tokens', 'once');
        ok = ~isempty(tok) && str2double(tok{1}) == n && strcmp(tok{3}, 'OK');
        if ok
            c = crc32Of(data);
            if ~isnan(c)
                ok = c == hex2dec(tok{2});
            end
        end
        msg = tail;
    end

    function c = crc32Of(data)
        % zlib CRC-32 (the same as esp_rom_crc32_le with start value 0). NaN if Java is not available.
        try
            j = java.util.zip.CRC32;
            j.update(typecast(data(:), 'int8'));
            c = double(j.getValue());
        catch
            c = NaN;
        end
    end

    % =====================================================================
    % --- Files & Plot window ---
    % =====================================================================
    function openFilesWindow(~, ~)
        if ~isempty(filesFig) && isvalid(filesFig)
            figure(filesFig);
            return;
        end
        filesFig = uifigure('Name', 'DAQ Files & Plot', 'Position', [150, 80, 1160, 720]);

        % Files on the connected DAQ (NAND = PFS, micro SD = SD)
        pnlB = uipanel(filesFig, 'Title', 'Files on the DAQ (NAND / micro SD)', 'FontWeight', 'bold', 'Position', [10, 370, 340, 340]);
        filesInfoLbl = uilabel(pnlB, 'Position', [10, 290, 320, 22], 'Text', 'Connected DAQ: none');
        boardList = uilistbox(pnlB, 'Position', [10, 45, 320, 240], 'Items', {}, 'Multiselect', 'on');
        uibutton(pnlB, 'Position', [10, 10, 100, 26], 'Text', 'Refresh List', 'ButtonPushedFcn', @refreshBoardFiles);
        uibutton(pnlB, 'Position', [120, 10, 210, 26], 'Text', 'Download Selected', 'ButtonPushedFcn', @downloadSelected);

        % Downloaded files in the local folder
        pnlL = uipanel(filesFig, 'Title', 'Downloaded Files (select one to plot)', 'FontWeight', 'bold', 'Position', [10, 10, 340, 350]);
        dirField = uieditfield(pnlL, 'text', 'Position', [10, 295, 235, 22], 'Value', downloadDir, ...
            'ValueChangedFcn', @(src, ~) setDownloadDir(src.Value));
        uibutton(pnlL, 'Position', [250, 295, 80, 22], 'Text', 'Browse...', 'ButtonPushedFcn', @browseDir);
        localList = uilistbox(pnlL, 'Position', [10, 45, 320, 240], 'Items', {}, 'ValueChangedFcn', @plotSelected);
        uibutton(pnlL, 'Position', [10, 10, 100, 26], 'Text', 'Rescan Folder', 'ButtonPushedFcn', @(~, ~) refreshLocalFiles());
        uibutton(pnlL, 'Position', [120, 10, 210, 26], 'Text', 'Plot Selected', 'ButtonPushedFcn', @plotSelected);

        % Plot with start / end range sliders
        axF = uiaxes(filesFig, 'Position', [360, 150, 790, 560]);
        grid(axF, 'on');
        title(axF, 'Select a downloaded file');
        xlabel(axF, 'Time (s)');
        ylabel(axF, 'Voltage (V)');

        uilabel(filesFig, 'Position', [360, 110, 45, 22], 'Text', 'Start:');
        startSlider = uislider(filesFig, 'Position', [410, 120, 600, 3], 'Limits', [0, 1], 'Value', 0, ...
            'ValueChangedFcn', @rangeChanged, 'ValueChangingFcn', @rangeChanging);
        startLbl = uilabel(filesFig, 'Position', [1030, 110, 120, 22], 'Text', '0.000 s');

        uilabel(filesFig, 'Position', [360, 60, 45, 22], 'Text', 'End:');
        endSlider = uislider(filesFig, 'Position', [410, 70, 600, 3], 'Limits', [0, 1], 'Value', 1, ...
            'ValueChangedFcn', @rangeChanged, 'ValueChangingFcn', @rangeChanging);
        endLbl = uilabel(filesFig, 'Position', [1030, 60, 120, 22], 'Text', '1.000 s');

        uilabel(filesFig, 'Position', [360, 12, 45, 22], 'Text', 'Units:');
        unitDrop = uidropdown(filesFig, 'Position', [410, 12, 170, 22], ...
            'Items', {'Voltage (V)', 'Pressure (PSI, Ax+B)', 'Raw counts'}, 'ValueChangedFcn', @(~, ~) updateRangePlot());
        statsLbl = uilabel(filesFig, 'Position', [590, 12, 560, 22], 'Text', '');

        updateChainLabel();
        refreshLocalFiles();
        if ~isempty(s) && isvalid(s)
            refreshBoardFiles();
        end
    end

    function setDownloadDir(d)
        downloadDir = d;
        refreshLocalFiles();
    end

    function browseDir(~, ~)
        d = uigetdir(downloadDir, 'Download folder');
        figure(filesFig);
        if ischar(d)
            dirField.Value = d;
            setDownloadDir(d);
        end
    end

    function refreshBoardFiles(~, ~)
        if isempty(s) || ~isvalid(s)
            uialert(filesFig, 'Connect to a DAQ first (Auto-Connect).', 'Not Connected');
            return;
        end
        try
            boardFiles = queryFiles();
        catch ME
            uialert(filesFig, ME.message, 'File List Error');
            return;
        end
        items = cell(1, numel(boardFiles));
        for k = 1:numel(boardFiles)
            f = boardFiles(k);
            if strcmp(f.src, 'PFS')
                items{k} = sprintf('NAND  file %s  (DAQ%d)  %s', f.name, selfId, sizeStr(f.bytes));
            else
                dev = regexp(f.name, 'D(\d)\.BIN$', 'tokens', 'once');
                if isempty(dev), dev = {'?'}; end
                items{k} = sprintf('uSD   %s  (DAQ%s)  %s', f.name, dev{1}, sizeStr(f.bytes));
            end
        end
        boardList.Items = items;
    end

    function downloadSelected(~, ~)
        if isempty(s) || ~isvalid(s)
            uialert(filesFig, 'Connect to a DAQ first (Auto-Connect).', 'Not Connected');
            return;
        end
        sel = find(ismember(boardList.Items, boardList.Value));
        if isempty(sel)
            uialert(filesFig, 'Select one or more files in the list.', 'Nothing Selected');
            return;
        end
        if ~exist(downloadDir, 'dir')
            mkdir(downloadDir);
        end
        dlg = uiprogressdlg(filesFig, 'Title', 'Download', 'Message', '');
        bad = {};
        for k = 1:numel(sel)
            f = boardFiles(sel(k));
            if strcmp(f.src, 'PFS')
                localName = sprintf('NAND_F%05dD%d.BIN', str2double(f.name), selfId);
            else
                localName = ['uSD_', f.name];
            end
            dlg.Message = sprintf('%d / %d: %s', k, numel(sel), localName);
            dlg.Value = 0;
            try
                [data, ok, msg] = getFile(f.src, f.name, dlg);
            catch ME
                data = uint8([]); ok = false; msg = ME.message;
            end
            if ~isempty(data)
                fid = fopen(fullfile(downloadDir, localName), 'w');
                fwrite(fid, data, 'uint8');
                fclose(fid);
            end
            if ~ok
                bad{end+1} = sprintf('%s: %s', localName, msg); %#ok<AGROW>
            end
        end
        close(dlg);
        refreshLocalFiles();
        if ~isempty(bad)
            uialert(filesFig, strjoin(bad, newline), 'Download Problem');
        end
    end

    function refreshLocalFiles()
        if isempty(localList) || ~isvalid(localList)
            return;
        end
        d = dir(fullfile(downloadDir, '*.BIN'));
        localList.Items = {d.name};
    end

    function plotSelected(~, ~)
        name = localList.Value;
        if isempty(name)
            return;
        end
        [plotT, plotRaw, plotSeq] = loadRecords(fullfile(downloadDir, name));
        plotName = name;
        if isempty(plotT)
            cla(axF);
            title(axF, [name, ' (no records)'], 'Interpreter', 'none');
            statsLbl.Text = '';
            return;
        end
        tEnd = max(plotT(end), 1e-3);
        startSlider.Limits = [0, tEnd];
        endSlider.Limits = [0, tEnd];
        startSlider.Value = 0;
        endSlider.Value = tEnd;
        updateRangePlot();
    end

    function [t, raw, seq] = loadRecords(path)
        % 8-byte records, little endian: u32 timestamp_us, i16 reading, u16 seq
        fid = fopen(path, 'r');
        bytes = fread(fid, inf, 'uint8=>uint8');
        fclose(fid);
        n = floor(numel(bytes) / 8);
        if n == 0
            t = []; raw = []; seq = [];
            return;
        end
        r = reshape(bytes(1:n*8), 8, n);
        ts = double(typecast(reshape(r(1:4, :), [], 1), 'uint32'));
        raw = double(typecast(reshape(r(5:6, :), [], 1), 'int16'));
        seq = double(typecast(reshape(r(7:8, :), [], 1), 'uint16'));
        dt = diff(ts);
        dt(dt < 0) = dt(dt < 0) + 2^32; % the timestamp wraps after 71 min
        t = [0; cumsum(dt)] / 1e6;
    end

    function rangeChanging(src, evt)
        if src == startSlider
            startLbl.Text = sprintf('%.3f s', evt.Value);
        else
            endLbl.Text = sprintf('%.3f s', evt.Value);
        end
    end

    function rangeChanged(src, ~)
        % Keep start < end: push the other slider
        gap = 1e-3 * diff(startSlider.Limits);
        if startSlider.Value > endSlider.Value - gap
            if src == startSlider
                endSlider.Value = min(endSlider.Limits(2), startSlider.Value + gap);
            else
                startSlider.Value = max(0, endSlider.Value - gap);
            end
        end
        updateRangePlot();
    end

    function updateRangePlot()
        if isempty(axF) || ~isvalid(axF) || isempty(plotT)
            return;
        end
        t0 = startSlider.Value;
        t1 = endSlider.Value;
        startLbl.Text = sprintf('%.3f s', t0);
        endLbl.Text = sprintf('%.3f s', t1);
        k = find(plotT >= t0 & plotT <= t1);
        if isempty(k)
            cla(axF);
            statsLbl.Text = 'No samples in the range';
            return;
        end
        switch unitDrop.Value
            case 'Raw counts'
                y = plotRaw; yl = 'ADS1115 counts';
            case 'Pressure (PSI, Ax+B)'
                y = currentA .* (plotRaw * 0.1875e-3) + currentB; yl = 'Pressure (PSI)';
            otherwise
                y = plotRaw * 0.1875e-3; yl = 'Voltage (V)';
        end
        yk = y(k);
        tk = plotT(k);

        % More than maxPts samples: plot the min and max of each bucket (keeps the spikes visible)
        maxPts = 20000;
        m = numel(k);
        if m > maxPts
            step = ceil(m / (maxPts / 2));
            nb = floor(m / step);
            yb = reshape(yk(1:nb*step), step, nb);
            tb = reshape(tk(1:nb*step), step, nb);
            xp = reshape([tb(1, :); tb(end, :)], [], 1);
            yp = reshape([min(yb, [], 1); max(yb, [], 1)], [], 1);
        else
            xp = tk;
            yp = yk;
        end
        cla(axF);
        plot(axF, xp, yp, 'b-');
        xlim(axF, [t0, max(t1, t0 + 1e-6)]);
        title(axF, plotName, 'Interpreter', 'none');
        xlabel(axF, 'Time (s)');
        ylabel(axF, yl);
        grid(axF, 'on');

        ds = mod(diff(plotSeq(k)), 65536);
        lost = sum(ds(ds ~= 1) - 1);
        statsLbl.Text = sprintf('%d samples  %.3f s   mean %.4f  min %.4f  max %.4f  std %.4f   lost samples %d', ...
            m, tk(end) - tk(1), mean(yk), min(yk), max(yk), std(yk), lost);
    end

    function str = sizeStr(b)
        if b >= 1048576
            str = sprintf('%.1f MB', b / 1048576);
        elseif b >= 1024
            str = sprintf('%.1f KB', b / 1024);
        else
            str = sprintf('%d B', b);
        end
    end

    % --- Cleanup on Close ---
    fig.CloseRequestFcn = @(~, ~) closeApp();

    % A compiled app ends when this function returns: wait for the window to close
    if isdeployed
        waitfor(fig);
    end
    function closeApp()
        if ~isempty(daqTimer) && isvalid(daqTimer)
            stop(daqTimer);
            delete(daqTimer);
        end
        if ~isempty(chainTimer) && isvalid(chainTimer)
            stop(chainTimer);
            delete(chainTimer);
        end
        if ~isempty(filesFig) && isvalid(filesFig)
            delete(filesFig);
        end
        if ~isempty(s) && isvalid(s)
            clear s;
        end
        if liveDataFile ~= -1
            fclose(liveDataFile);
        end
        if calDataFile ~= -1
            fclose(calDataFile);
        end
        if ~isempty(calFig) && isvalid(calFig)
            delete(calFig);
        end
        delete(fig);
    end
end