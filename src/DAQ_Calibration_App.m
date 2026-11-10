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

    % --- Sensor of the live view: 1 = PT, 2 = load cell ---
    % The live plots, the formula, and the calibration points belong to one sensor. The values of the other sensor
    % wait in sensorSaved. The DAQ saves both sensors at all times.
    sensorIdx = 1;
    sensorSaved = struct('calV', {[], []}, 'calP', {[], []}, 'A', {1, 1}, 'B', {0, 0});
    liveDecimation = [64, 16];       % about 14 points/s for the PT, about 20 points/s for the load cell
    lcMvVPerCount = 1000 / 2^31;     % load cell output: counts / (gain 128 * 2^24), as mV/V
    xUnit = {'V', 'mV/V'};
    yUnit = {'PSI', 'N'};

    % --- DAQ chain and file transfer state ---
    chainTimer = [];   % Timer that asks the DAQ for the boards in the UART chain (DAQS)
    selfId = -1;       % Device ID of the DAQ on USB (-1 = unknown)
    seenIds = [];      % Device IDs of the other DAQs in the chain
    portName = '';     % Serial port of the connected DAQ
    xferBusy = false;  % True while a FILES/CHUNK transfer owns the serial port
    calReplyMode = ''; % 'save' or 'read': what the next CAL:OK reply of the DAQ is for ('' = no button waits)

    % Files & Plot window handles and data
    filesFig = []; boardList = []; localList = []; dirField = []; filesInfoLbl = []; sensorDropF = [];
    axF = []; warnLbl = []; startSlider = []; endSlider = []; startLbl = []; endLbl = []; statsLbl = []; unitDrop = [];
    calLbl = [];
    boardFiles = struct('src', {}, 'name', {}, 'bytes', {}, 'start', {});
    plotT = []; plotRaw = []; plotSeq = []; plotName = ''; plotBytes = 0;
    plotSets = struct('t', {[], []}, 'raw', {[], []}, 'seq', {[], []}); % one entry for each sensor of the loaded file
    plotSensor = 1;
    plotCal = noCal(); % the calibration block that loadRecords found in the loaded file
    unitItems = {{'Voltage (V)', 'Pressure (PSI, Ax+B)', 'Raw counts'}, {'Output (mV/V)', 'Force (N, Ax+B)', 'Raw counts'}};

    % Handles for performant live plotting and windows
    lineV = [];
    lineP = [];
    calFig = [];           % Handle for the calibration window
    wifiFig = []; wifiF = struct();   % WiFi / Telemetry window and its fields
    wifiPassSet = false;   % the DAQ has a WiFi password (it never sends the password back)
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
    otherLiveFile = -1;   % the CSV files of the sensor that the live view does not show
    otherCalFile = -1;
    lcFilesOpen = false;  % the load cell CSV files open when the load cell is selected

    % --- Main UI Layout ---
    fig = uifigure('Name', 'DAQ Calibration Tool', 'Position', [100, 100, 1100, 710]);

    % DAQ chain row: the boards on the UART chain and the one on USB
    chainLbl = uilabel(fig, 'Position', [20, 675, 760, 22], 'Text', 'DAQs in chain: (not connected)', 'FontWeight', 'bold');
    btnFiles = uibutton(fig, 'Position', [940, 675, 140, 22], 'Text', 'Files & Plot...', 'ButtonPushedFcn', @openFilesWindow);
    btnWifi = uibutton(fig, 'Position', [790, 675, 140, 22], 'Text', 'WiFi / Telemetry...', 'ButtonPushedFcn', @openWifiWindow);
    downloadDir = fullfile(outDir, 'Downloads');

    % Status, Connection, and Window Size Controls
    statusLbl = uilabel(fig, 'Position', [20, 640, 340, 22], 'Text', 'Status: Disconnected', 'FontWeight', 'bold');
    btnConnect = uibutton(fig, 'Position', [370, 640, 100, 22], 'Text', 'Auto-Connect', 'ButtonPushedFcn', @connectDAQ);

    uilabel(fig, 'Position', [510, 640, 130, 22], 'Text', 'Live Samples Shown:');
    windowSlider = uislider(fig, 'Position', [650, 650, 180, 3], 'Limits', [10, 500], 'Value', windowSize, 'ValueChangedFcn', @updateWindowSize);

    uilabel(fig, 'Position', [860, 640, 50, 22], 'Text', 'Sensor:');
    uidropdown(fig, 'Position', [915, 640, 165, 22], 'Items', {'PT', 'Load Cell'}, 'ValueChangedFcn', @sensorChanged);

    % --- Section 1: Old Calibration Buttons ---
    pnlCal = uipanel(fig, 'Title', 'PT Calibration (Point-by-Point)', 'FontWeight', 'bold', 'Position', [20, 560, 1060, 65]);

    lblSetpoint = uilabel(pnlCal, 'Position', [15, 12, 210, 22], 'Text', 'Regulator Pressure Setpoint (PSI):');
    numInput = uieditfield(pnlCal, 'numeric', 'Position', [230, 12, 70, 22], 'Value', 0);
    btnCapture = uibutton(pnlCal, 'Position', [315, 12, 160, 22], 'Text', 'Capture Average & Save', 'ButtonPushedFcn', @captureCal);
    btnViewCal = uibutton(pnlCal, 'Position', [490, 12, 150, 22], 'Text', 'Calculate fit & View', 'ButtonPushedFcn', @openCalWindow);

    % --- Section 2: Manual Ax+B Formula & Zero Pressure ---
    pnlParams = uipanel(fig, 'Title', 'Manual Formula (Ax + B) & Zeroing', 'FontWeight', 'bold', 'Position', [20, 462, 1060, 88]);

    lblFormula = uilabel(pnlParams, 'Position', [15, 12, 110, 22], 'Text', 'Pressure (PSI) =');
    inputA = uieditfield(pnlParams, 'numeric', 'Position', [125, 12, 85, 22], ...
        'Value', currentA, 'ValueDisplayFormat', '%.5f', 'ValueChangedFcn', @updateNumericAB);

    lblTimes = uilabel(pnlParams, 'Position', [218, 12, 40, 22], 'Text', '* V +');
    inputB = uieditfield(pnlParams, 'numeric', 'Position', [260, 12, 85, 22], ...
        'Value', currentB, 'ValueDisplayFormat', '%.5f', 'ValueChangedFcn', @updateNumericAB);

    btnZeroAtm = uibutton(pnlParams, 'Position', [365, 12, 190, 22], ...
        'Text', 'Zero Out Pressure (Tare)', 'ButtonPushedFcn', @zeroPressure);

    tareStatusLbl = uilabel(pnlParams, 'Position', [570, 12, 480, 22], 'Text', '');

    % Second row of the panel: A and B of the selected sensor go to the flash of the DAQ and come back from it
    btnSaveCal = uibutton(pnlParams, 'Position', [815, 38, 110, 22], ...
        'Text', 'Save to DAQ', 'ButtonPushedFcn', @saveCalToDaq);
    btnReadCal = uibutton(pnlParams, 'Position', [935, 38, 115, 22], ...
        'Text', 'Read from DAQ', 'ButtonPushedFcn', @readCalFromDaq);

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

    % --- Selection of the live sensor ---
    function sensorChanged(src, ~)
        k = find(strcmp(src.Items, src.Value), 1);
        if k == sensorIdx
            return;
        end
        sensorSaved(sensorIdx).calV = calV;
        sensorSaved(sensorIdx).calP = calP;
        sensorSaved(sensorIdx).A = currentA;
        sensorSaved(sensorIdx).B = currentB;
        sensorIdx = k;
        calV = sensorSaved(k).calV;
        calP = sensorSaved(k).calP;
        currentA = sensorSaved(k).A;
        currentB = sensorSaved(k).B;
        inputA.Value = currentA;
        inputB.Value = currentB;
        tareStatusLbl.Text = '';

        tmp = liveDataFile; liveDataFile = otherLiveFile; otherLiveFile = tmp;
        tmp = calDataFile; calDataFile = otherCalFile; otherCalFile = tmp;
        if k == 2 && ~lcFilesOpen
            lcFilesOpen = true;
            if isLiveLogging
                liveDataFile = fopen(fullfile(outDir, sprintf('live_data_loadcell_%s.csv', timestampStr)), 'a');
                fprintf(liveDataFile, 'Time,mV/V\n');
            end
            calDataFile = fopen(fullfile(outDir, sprintf('manual_calibration_points_loadcell_%s.csv', timestampStr)), 'a');
            fprintf(calDataFile, 'mV/V,Force\n');
        end

        % The plots start again with the samples of the new sensor
        decimationFactor = liveDecimation(k);
        sampleCount = 0;
        liveV = [];
        liveT = [];
        lineV.XData = NaN; lineV.YData = NaN;
        lineP.XData = NaN; lineP.YData = NaN;
        if ~isempty(calFig) && isvalid(calFig)
            delete(calFig);
        end
        calFig = [];
        calTransientLine = [];

        if k == 1
            pnlCal.Title = 'PT Calibration (Point-by-Point)';
            lblSetpoint.Text = 'Regulator Pressure Setpoint (PSI):';
            lblFormula.Text = 'Pressure (PSI) =';
            lblTimes.Text = '* V +';
            btnZeroAtm.Text = 'Zero Out Pressure (Tare)';
            title(ax1, 'Live Voltage (Transducer)');
            ylabel(ax1, 'Voltage (V)');
            ylim(ax1, [0, 5.5]);
            title(ax2, 'Live Estimated Pressure (PSI)');
            ylabel(ax2, 'Pressure (PSI)');
        else
            pnlCal.Title = 'Load Cell Calibration (Point-by-Point)';
            lblSetpoint.Text = 'Applied Force (N):';
            lblFormula.Text = 'Force (N) =';
            lblTimes.Text = '* mV/V +';
            btnZeroAtm.Text = 'Zero Out Force (Tare)';
            title(ax1, 'Live Output (Load Cell)');
            ylabel(ax1, 'Output (mV/V)');
            ylim(ax1, 'auto');
            title(ax2, 'Live Estimated Force (N)');
            ylabel(ax2, 'Force (N)');
        end
    end

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
                    sprintf('%s = %.5f * %s %+.5f\n', yUnit{sensorIdx}, currentA, xUnit{sensorIdx}, currentB), ...
                    '--- Non-Linearity Errors ---' ...
                };
                for idx = 1:length(calV)
                    lines{end+1} = sprintf('Pt %d (%.2f %s Target):', idx, calP(idx), yUnit{sensorIdx});
                    lines{end+1} = sprintf('  Error:    %+.4f %s', errs(idx), yUnit{sensorIdx});
                    lines{end+1} = sprintf('  Sq Error:  %.4e %s^2', sq_errs(idx), yUnit{sensorIdx});
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
            uialert(fig, 'No live sensor data available to zero the offset.', 'Error');
            return;
        end

        % Calculate steady-state average output from last 10 decimated points
        ptsToAvg = min(10, length(liveV));
        avgV = mean(liveV(end-ptsToAvg+1:end));

        % Solve for B such that A * avgV + B = 0
        currentB = -currentA * avgV;
        inputB.Value = currentB;

        % Update non-blocking status label instead of stalling serial queue with uialert
        tareStatusLbl.Text = sprintf('Zeroed at %.5f %s (New B = %+.5f %s)', avgV, xUnit{sensorIdx}, currentB, yUnit{sensorIdx});

        updatePlotsOnly();
        updateCalWindowDetails();
    end

    function connectDAQ(~, ~)
        if xferBusy
            return; % a file transfer uses the port
        end
        % Clean up any prior connection
        if ~isempty(daqTimer) && isvalid(daqTimer)
            stop(daqTimer);
            delete(daqTimer);
        end
        if ~isempty(chainTimer) && isvalid(chainTimer)
            stop(chainTimer);
            delete(chainTimer);
        end
        closePort();
        selfId = -1;
        seenIds = [];
        updateChainLabel();

        statusLbl.Text = 'Status: Scanning...';
        drawnow;
        ports = serialportlist("available");

        for i = 1:length(ports)
            temp_s = [];
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
                delete(temp_s); % not the DAQ: free the port
            catch
                % Ignore and inspect next port
                if ~isempty(temp_s) && isvalid(temp_s)
                    delete(temp_s);
                end
            end
        end
        statusLbl.Text = 'Status: DAQ Not Found';
    end

    function readSerialBatch(~, ~)
        try
            if xferBusy || isempty(s) || ~isvalid(s)
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

            % Load cell lines "L:<raw count>" come between the PT number lines
            lcCounts = [];
            % (a text line such as "CAL:OK ..." also contains "L:": look for the line start)
            if ~isempty(regexp(completeLines, '^L:', 'once', 'lineanchors'))
                lcTok = regexp(completeLines, '^L:(-?\d+)', 'tokens', 'lineanchors');
                lcCounts = sscanf(strjoin([lcTok{:}], ' '), '%f');
                completeLines = regexprep(completeLines, '^L:-?\d+', '', 'lineanchors');
            end

            % Text lines (replies such as DAQS:..., log lines) come between the number lines.
            % A text line starts with a character that cannot start a number.
            txtRe = '^[^0-9+\-.\r\n][^\r\n]*';
            txtLines = regexp(completeLines, txtRe, 'match', 'lineanchors');
            if ~isempty(txtLines)
                for k = 1:numel(txtLines)
                    handleReply(strtrim(txtLines{k}));
                end
                completeLines = regexprep(completeLines, txtRe, '', 'lineanchors');
            end

            % Vectorized C-speed float parsing of all samples in this chunk
            vals = sscanf(completeLines, '%f');
            if sensorIdx == 2
                vals = lcCounts * lcMvVPerCount; % the plots show the selected sensor only
            end
            if isempty(vals)
                return;
            end

            % Clamp voltage strictly between 0 and 5.5V
            if sensorIdx == 1
                vals = max(0, min(5.5, vals));
            end

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
                        fprintf(liveDataFile, '%s,%.7f\n', t_stamp, decV(k));
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
                closePort();
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
            fprintf(calDataFile, '%.7f,%f\n', avgV, pSet);
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

        uialert(fig, sprintf('Captured %.5f %s at %.2f %s.', avgV, xUnit{sensorIdx}, pSet, yUnit{sensorIdx}), 'Saved', 'Icon', 'success');
    end

    function openCalWindow(~, ~)
        if length(calV) < 2
            uialert(fig, 'Need at least 2 manual points to least-squares fit a line.', 'Not Enough Data');
            return;
        end
        if isempty(liveV)
            uialert(fig, 'No live data available yet.', 'Error');
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

        if sensorIdx == 1
            v_min = max(0, min([calV, liveV]) - 0.5);
            v_max = min(5.5, max([calV, liveV]) + 0.5);
        else
            v_span = max(max([calV, liveV]) - min([calV, liveV]), 1e-6);
            v_min = min([calV, liveV]) - 0.1 * v_span;
            v_max = max([calV, liveV]) + 0.1 * v_span;
        end

        v_range = linspace(v_min, v_max, 100);
        p_line = currentA .* v_range + currentB;
        calFitLine = plot(axCal, v_range, p_line, 'k--', 'LineWidth', 1.5, 'DisplayName', 'ax+b Fit');

        plot(axCal, calV, calP, '^', 'MarkerFaceColor', 'b', 'MarkerEdgeColor', 'b', ...
            'MarkerSize', 8, 'DisplayName', 'Manual Pts');

        live_estP_current = currentA .* liveV(end) + currentB;
        calTransientLine = plot(axCal, liveV(end), live_estP_current, 'ko', ...
            'MarkerSize', 6, 'DisplayName', 'Live Transient');

        title(axCal, 'Calibration Linearity Check');
        xlabel(axCal, sprintf('Sensor Output (%s)', xUnit{sensorIdx}));
        ylabel(axCal, sprintf('Estimated Value (%s)', yUnit{sensorIdx}));
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
            idText = regexp(tok{2}, '\d+', 'match'); % SEEN can be empty
            seenIds = zeros(1, numel(idText));
            for k = 1:numel(idText)
                seenIds(k) = str2double(idText{k});
            end
            updateChainLabel();
        end

        % Reply of CAL PT / CAL LC / CAL GET: "CAL:OK rev=.. pt_set=.. pt_a=.. ..." or "CAL:ERR <reason>"
        if startsWith(t, 'CAL:ERR')
            calReplyMode = '';
            tareStatusLbl.Text = ['DAQ error: ', strtrim(regexprep(t, '^CAL:ERR', ''))];
        elseif startsWith(t, 'CAL:OK')
            applyCalReply(t);
        end

        % Reply of WIFI GET / WIFI MODE / WIFI SSID / WIFI PASS / TELEM SERVER / TELEM DECIM / REBOOT
        if startsWith(t, 'WIFI:') || startsWith(t, 'REBOOT:')
            applyWifiReply(t);
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
    % --- Calibration in the DAQ (CAL PT / CAL LC / CAL GET commands) ---
    % "CAL PT <A> <B>" or "CAL LC <A> <B>" saves A and B of one sensor in the DAQ, "CAL GET" asks for both.
    % Reply (one line): "CAL:OK rev=<n> pt_set=<0|1> pt_a=<f> pt_b=<f> lc_set=<0|1> lc_a=<f> lc_b=<f>" or "CAL:ERR <text>"
    % =====================================================================
    function ok = daqReadyForCal()
        % The checks of both buttons: a connected DAQ, and no file transfer on the port
        ok = false;
        if isempty(s) || ~isvalid(s)
            uialert(fig, 'Connect to a DAQ first (Auto-Connect).', 'Not Connected');
            return;
        end
        if xferBusy
            return;
        end
        ok = true;
    end

    function saveCalToDaq(~, ~)
        if ~daqReadyForCal()
            return;
        end
        if ~isfinite(currentA) || ~isfinite(currentB)
            uialert(fig, 'A and B must be finite numbers.', 'Cannot Save');
            return;
        end
        if sensorIdx == 1
            tag = 'PT';
        else
            tag = 'LC';
        end
        calReplyMode = 'save';
        tareStatusLbl.Text = 'Saving to DAQ...';
        try
            writeline(s, sprintf('CAL %s %.9g %.9g', tag, currentA, currentB));
        catch
            calReplyMode = '';
            tareStatusLbl.Text = 'Could not send the command to the DAQ.';
        end
    end

    function readCalFromDaq(~, ~)
        if ~daqReadyForCal()
            return;
        end
        calReplyMode = 'read';
        tareStatusLbl.Text = 'Reading from DAQ...';
        try
            writeline(s, "CAL GET");
        catch
            calReplyMode = '';
            tareStatusLbl.Text = 'Could not send the command to the DAQ.';
        end
    end

    function applyCalReply(t)
        tok = regexp(t, ['^CAL:OK\s+rev=(\d+)\s+pt_set=(\d)\s+pt_a=(\S+)\s+pt_b=(\S+)', ...
            '\s+lc_set=(\d)\s+lc_a=(\S+)\s+lc_b=(\S+)'], 'tokens', 'once');
        forBtn = calReplyMode;   % which button asked
        calReplyMode = '';   % one reply for each button press
        if isempty(tok)
            tareStatusLbl.Text = ['DAQ reply not understood: ', t];
            return;
        end
        v = str2double(tok);
        c = noCal();
        c.valid = true;
        c.rev = v(1);
        c.ptSet = v(2) ~= 0; c.ptA = v(3); c.ptB = v(4);
        c.lcSet = v(5) ~= 0; c.lcA = v(6); c.lcB = v(7);
        if strcmp(forBtn, 'save')
            tareStatusLbl.Text = ['DAQ saved: ', calText(c, ' | ')];
            return;   % the values of the app stay as they are
        elseif strcmp(forBtn, 'read')
            tareStatusLbl.Text = ['Read from DAQ: ', calText(c, ' | ')];
        else
            tareStatusLbl.Text = ['DAQ calibration: ', calText(c, ' | ')];
            return;
        end

        % Read from DAQ: take the values of the sensors that have a saved calibration
        isSet = [c.ptSet, c.lcSet];
        newA = [c.ptA, c.lcA];
        newB = [c.ptB, c.lcB];
        for kk = 1:2
            if ~isSet(kk) || ~isfinite(newA(kk)) || ~isfinite(newB(kk))
                continue;
            end
            if kk == sensorIdx
                currentA = newA(kk);
                currentB = newB(kk);
                inputA.Value = currentA;
                inputB.Value = currentB;
            else
                sensorSaved(kk).A = newA(kk);
                sensorSaved(kk).B = newB(kk);
            end
        end
        updatePlotsOnly();
        updateCalWindowDetails();
    end

    function c = noCal()
        % A calibration block with nothing in it (the same fields as the result of loadRecords)
        c = struct('valid', false, 'rev', 0, 'ptSet', false, 'ptA', NaN, 'ptB', NaN, ...
            'lcSet', false, 'lcA', NaN, 'lcB', NaN, 'nBlocks', 0);
    end

    function str = calText(c, sep)
        % "rev 3, PT A=.., B=.." for the sensors that have a calibration; sep separates the sensors
        parts = {};
        if c.ptSet
            parts{end+1} = sprintf('PT A=%.6g, B=%.6g', c.ptA, c.ptB);
        end
        if c.lcSet
            parts{end+1} = sprintf('LC A=%.6g, B=%.6g', c.lcA, c.lcB);
        end
        if isempty(parts)
            parts = {'no sensor calibrated'};
        end
        str = sprintf('rev %d, %s', c.rev, strjoin(parts, sep));
    end

    % =====================================================================
    % --- WiFi / Telemetry window (WIFI, TELEM and REBOOT commands) ---
    % The mode and the settings go to the flash of the DAQ that is on USB. They start after the next reboot of that DAQ.
    %   OFF      no WiFi at all (the DAQ only records)
    %   UPLOAD   the DAQ near the rocket: sends every Nth sample to the server
    %   DOWNLOAD the DAQ in the bunker: gets the data from the server and prints it (view it with DAQ_Live_Monitor)
    % =====================================================================
    function openWifiWindow(~, ~)
        if ~isempty(wifiFig) && isvalid(wifiFig)
            figure(wifiFig);
            return;
        end
        wifiFig = uifigure('Name', 'WiFi / Telemetry of the connected DAQ', 'Position', [250, 150, 560, 470]);
        y = 425;
        uilabel(wifiFig, 'Position', [20, y, 130, 22], 'Text', 'WiFi mode:');
        wifiF.mode = uidropdown(wifiFig, 'Position', [160, y, 160, 22], 'Items', {'OFF', 'UPLOAD', 'DOWNLOAD'}, 'Value', 'OFF');
        y = y - 35;
        uilabel(wifiFig, 'Position', [20, y, 130, 22], 'Text', 'Hotspot name (SSID):');
        wifiF.ssid = uieditfield(wifiFig, 'text', 'Position', [160, y, 380, 22]);
        y = y - 30;
        uilabel(wifiFig, 'Position', [20, y, 130, 22], 'Text', 'Hotspot password:');
        wifiF.pass = uieditfield(wifiFig, 'text', 'Position', [160, y, 250, 22]);
        wifiF.clearPass = uicheckbox(wifiFig, 'Position', [420, y, 130, 22], 'Text', 'No password');
        y = y - 30;
        wifiF.passInfo = uilabel(wifiFig, 'Position', [160, y, 380, 22], 'Text', '', 'FontAngle', 'italic');
        y = y - 30;
        uilabel(wifiFig, 'Position', [20, y, 130, 22], 'Text', 'Server host / IP:');
        wifiF.host = uieditfield(wifiFig, 'text', 'Position', [160, y, 250, 22]);
        uilabel(wifiFig, 'Position', [420, y, 40, 22], 'Text', 'Port:');
        wifiF.port = uieditfield(wifiFig, 'numeric', 'Position', [460, y, 80, 22], 'Value', 48613, ...
            'Limits', [1 65535], 'RoundFractionalValues', 'on', 'ValueDisplayFormat', '%d');
        y = y - 30;
        uilabel(wifiFig, 'Position', [20, y, 130, 22], 'Text', 'Server token:');
        wifiF.token = uieditfield(wifiFig, 'text', 'Position', [160, y, 250, 22]);
        y = y - 30;
        wifiF.tokenInfo = uilabel(wifiFig, 'Position', [160, y, 380, 22], 'Text', '(16 characters at most. Empty = keep the token in the DAQ.)', 'FontAngle', 'italic');
        y = y - 30;
        uilabel(wifiFig, 'Position', [20, y, 130, 22], 'Text', 'Keep every Nth:');
        uilabel(wifiFig, 'Position', [160, y, 30, 22], 'Text', 'PT');
        wifiF.dpt = uieditfield(wifiFig, 'numeric', 'Position', [190, y, 60, 22], 'Value', 15, 'Limits', [1 1000], ...
            'RoundFractionalValues', 'on', 'ValueDisplayFormat', '%d');
        uilabel(wifiFig, 'Position', [270, y, 80, 22], 'Text', 'Load cell');
        wifiF.dlc = uieditfield(wifiFig, 'numeric', 'Position', [340, y, 60, 22], 'Value', 5, 'Limits', [1 1000], ...
            'RoundFractionalValues', 'on', 'ValueDisplayFormat', '%d');
        y = y - 28;
        wifiF.rateInfo = uilabel(wifiFig, 'Position', [160, y, 380, 22], 'Text', '', 'FontAngle', 'italic');
        wifiF.dpt.ValueChangedFcn = @wifiRateText;
        wifiF.dlc.ValueChangedFcn = @wifiRateText;
        wifiRateText();
        y = y - 40;
        uibutton(wifiFig, 'Position', [20, y, 150, 28], 'Text', 'Read from DAQ', 'ButtonPushedFcn', @wifiRead);
        uibutton(wifiFig, 'Position', [185, y, 150, 28], 'Text', 'Send to DAQ', 'ButtonPushedFcn', @wifiSend);
        uibutton(wifiFig, 'Position', [350, y, 190, 28], 'Text', 'Reboot DAQ (applies it)', 'ButtonPushedFcn', @wifiReboot);
        y = y - 40;
        wifiF.status = uilabel(wifiFig, 'Position', [20, y - 40, 520, 70], 'Text', 'Press "Read from DAQ" first.', ...
            'VerticalAlignment', 'top', 'WordWrap', 'on');
        wifiFig.CloseRequestFcn = @(src, ~) delete(src);
    end

    function wifiRateText(~, ~)
        % The rates of the DAQ: PT 883 SPS, load cell 320 SPS
        wifiF.rateInfo.Text = sprintf('= %.1f Hz PT, %.1f Hz load cell to the server', 883 / wifiF.dpt.Value, 320 / wifiF.dlc.Value);
    end

    function ok = wifiReady()
        ok = daqReadyForCal();
        if ok && (isempty(wifiFig) || ~isvalid(wifiFig))
            ok = false;
        end
    end

    function wifiRead(~, ~)
        if ~wifiReady(), return; end
        wifiF.status.Text = 'Reading from the DAQ...';
        try
            writeline(s, "WIFI GET");
        catch
            wifiF.status.Text = 'Could not send the command to the DAQ.';
        end
    end

    function wifiSend(~, ~)
        if ~wifiReady(), return; end
        mode = wifiF.mode.Value;
        ssid = strtrim(wifiF.ssid.Value);
        host = strtrim(wifiF.host.Value);
        token = strtrim(wifiF.token.Value);
        pass = wifiF.pass.Value;
        if ~strcmp(mode, 'OFF') && (isempty(ssid) || isempty(host))
            uialert(wifiFig, 'UPLOAD and DOWNLOAD need the hotspot name and the server host.', 'Missing values');
            return;
        end
        if length(ssid) > 32 || length(pass) > 63 || length(token) > 16 || any(isspace(host)) || any(isspace(token)) || length(host) > 63
            uialert(wifiFig, 'SSID: 32 characters at most. Password: 63. Token: 16, no spaces. Host: 63, no spaces.', 'Value too long');
            return;
        end
        cmds = {};
        if ~isempty(ssid), cmds{end+1} = ['WIFI SSID ', ssid]; end
        if ~isempty(pass)
            cmds{end+1} = ['WIFI PASS ', pass];
        elseif wifiF.clearPass.Value
            cmds{end+1} = 'WIFI PASS';
        end
        if ~isempty(host)
            if isempty(token) && daqHasToken()
                uialert(wifiFig, 'Type the token again: the DAQ can only set the host, the port, and the token together.', 'Token needed');
                return;
            end
            cmds{end+1} = sprintf('TELEM SERVER %s %d %s', host, round(wifiF.port.Value), token);
        end
        cmds{end+1} = sprintf('TELEM DECIM %d %d', round(wifiF.dpt.Value), round(wifiF.dlc.Value));
        cmds{end+1} = ['WIFI MODE ', mode];
        wifiF.status.Text = 'Sending...';
        try
            for k = 1:numel(cmds)
                writeline(s, string(cmds{k}));
                pause(0.2);          % the DAQ writes each value to its flash
            end
            writeline(s, "WIFI GET");
        catch
            wifiF.status.Text = 'Could not send the commands to the DAQ.';
        end
    end

    function tf = daqHasToken()
        % The DAQ reports only that a token exists. With no token typed, the server command would delete it.
        tf = wifiF.tokenInfo.UserData;
        if isempty(tf), tf = false; end
    end

    function wifiReboot(~, ~)
        if ~wifiReady(), return; end
        sel = uiconfirm(wifiFig, ['The DAQ restarts and starts the saved WiFi mode. ', ...
            'It must not be recording. The USB link drops: wait about 5 s, then press Auto-Connect.'], 'Reboot the DAQ', ...
            'Options', {'Reboot', 'Cancel'}, 'DefaultOption', 'Cancel', 'CancelOption', 'Cancel');
        if ~strcmp(sel, 'Reboot'), return; end
        try
            writeline(s, "REBOOT");
            wifiF.status.Text = 'Reboot sent...';
        catch
            wifiF.status.Text = 'Could not send the command to the DAQ.';
        end
    end

    function applyWifiReply(t)
        if startsWith(t, 'WIFI:ERR') || startsWith(t, 'REBOOT:ERR')
            if ~isempty(wifiFig) && isvalid(wifiFig), wifiF.status.Text = ['DAQ error: ', strtrim(regexprep(t, '^(WIFI|REBOOT):ERR', ''))]; end
            return;
        end
        if startsWith(t, 'REBOOT:OK')
            if ~isempty(wifiFig) && isvalid(wifiFig), wifiF.status.Text = 'The DAQ restarts. Wait about 5 s, then press Auto-Connect.'; end
            return;
        end
        tok = regexp(t, ['^WIFI:OK\s+mode=(\w+)\s+run=(\w+)\s+port=(\d+)\s+host=(\S+)\s+token_set=(\d)\s+pass_set=(\d)', ...
            '\s+decim_pt=(\d+)\s+decim_lc=(\d+)\s+link=(\d)\s+wifi=(\d)\s+rssi=(-?\d+)\s+drops=(\d+)\s+sent=(\d+)', ...
            '\s+reconnects=(\d+)\s+ssid=(.*)$'], 'tokens', 'once');
        if isempty(tok) || isempty(wifiFig) || ~isvalid(wifiFig)
            return;
        end
        wifiPassSet = tok{6} == '1';
        wifiF.mode.Value = tok{1};
        wifiF.ssid.Value = tok{15};
        if ~strcmp(tok{4}, '-'), wifiF.host.Value = tok{4}; end
        wifiF.port.Value = str2double(tok{3});
        wifiF.dpt.Value = str2double(tok{7});
        wifiF.dlc.Value = str2double(tok{8});
        wifiF.pass.Value = '';
        wifiF.token.Value = '';
        wifiF.tokenInfo.UserData = tok{5} == '1';
        if wifiPassSet
            wifiF.passInfo.Text = '(a password is saved in the DAQ. Leave empty to keep it.)';
        else
            wifiF.passInfo.Text = '(no password saved in the DAQ)';
        end
        if tok{5} == '1'
            wifiF.tokenInfo.Text = '(a token is saved in the DAQ. Type it again to send the server settings.)';
        else
            wifiF.tokenInfo.Text = '(no token saved. 16 characters at most.)';
        end
        wifiRateText();
        link = {'no', 'yes'};
        msg = sprintf('Saved mode: %s. Running now: %s.', tok{1}, tok{2});
        if ~strcmp(tok{1}, tok{2})
            msg = [msg, ' The saved mode starts after a reboot (or the settings are not complete).'];
        end
        msg = sprintf('%s\nWiFi connected: %s. Link to the server: %s. RSSI %s dBm. Dropped %s. Frames sent %s. Reconnects %s.', msg, ...
            link{(tok{10} == '1') + 1}, link{(tok{9} == '1') + 1}, tok{11}, tok{12}, tok{13}, tok{14});
        wifiF.status.Text = msg;
    end

    % =====================================================================
    % --- Serial transfer helpers (FILES and CHUNK commands) ---
    % =====================================================================
    function closePort()
        if ~isempty(s) && isvalid(s)
            delete(s); % closes the port
        end
        s = [];
    end

    function pauseSerial()
        % Stop the timers and the live stream: after "STREAM:OFF" only replies come
        xferBusy = true;
        if ~isempty(daqTimer) && isvalid(daqTimer), stop(daqTimer); end
        if ~isempty(chainTimer) && isvalid(chainTimer), stop(chainTimer); end
        writeline(s, "STREAM OFF");
        waitLine({'STREAM:OFF'}, 5);
        flush(s);
        serialCarryover = '';
    end

    function resumeSerial()
        % Start the live stream and the timers again (also after an error)
        xferBusy = false;
        if isempty(s) || ~isvalid(s)
            return;
        end
        try
            flush(s);
            writeline(s, "STREAM ON");
        catch
        end
        serialCarryover = '';
        if ~isempty(daqTimer) && isvalid(daqTimer) && strcmp(char(daqTimer.Running), 'off'), start(daqTimer); end
        if ~isempty(chainTimer) && isvalid(chainTimer) && strcmp(char(chainTimer.Running), 'off'), start(chainTimer); end
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
        % Call between pauseSerial() and resumeSerial().
        % FILES reply: "FILE PFS <id> <bytes> <flags> <start>" and "FILE SD <name> <bytes>", then "FILES:END"
        files = struct('src', {}, 'name', {}, 'bytes', {}, 'start', {});
        writeline(s, "FILES");
        waitLine({'FILES:BEGIN'}, 5);
        while true
            ln = waitLine({'FILE ', 'FILES:END'}, 5);
            if strcmp(ln, 'FILES:END')
                break;
            end
            p = strsplit(ln);
            if numel(p) >= 4
                startOff = 0; % offset of the first whole record (PFS); SD files start at 0
                if numel(p) >= 6
                    startOff = str2double(p{6});
                end
                files(end+1) = struct('src', p{2}, 'name', p{3}, 'bytes', str2double(p{4}), 'start', startOff); %#ok<AGROW>
            end
        end
    end

    function [data, ok, msg, retries] = getFile(f, dlg)
        % Call between pauseSerial() and resumeSerial().
        % Ask for the file one chunk at a time; a chunk with an error is asked for again (up to maxTries times).
        chunkLen = 16384;
        maxTries = 5;
        approx = max(f.bytes, 1);
        buf = zeros(approx + 65536, 1, 'uint8');
        n = 0;
        retries = 0;
        off = f.start;
        while true
            tries = 0;
            while true
                [d, err] = getChunk(f, off, chunkLen);
                if isempty(err)
                    break;
                end
                tries = tries + 1;
                retries = retries + 1;
                if tries >= maxTries
                    data = buf(1:n);
                    ok = false;
                    msg = sprintf('%s (offset %d, %d tries)', err, off, tries);
                    return;
                end
                pause(0.3);   % let the rest of a bad chunk arrive, then drop it
                flush(s);
            end
            if isempty(d)
                break;        % a chunk with 0 bytes: the end of the file
            end
            m = numel(d);
            while n + m > numel(buf)
                buf(2 * numel(buf)) = 0; % grow
            end
            buf(n+1:n+m) = d;
            n = n + m;
            off = off + m;
            if ~isempty(dlg)
                dlg.Value = min(1, n / approx);
            end
        end
        data = buf(1:n);
        ok = true;
        msg = sprintf('OK, %d bytes', n);
    end

    function [d, err] = getChunk(f, off, len)
        % CHUNK reply: "CHUNK:<offset> <n> <crc32 hex>", then n raw bytes; "CHUNK:ERR <reason>" on an error
        d = uint8([]);
        err = '';
        writeline(s, sprintf('CHUNK %s %s %d %d', f.src, f.name, off, len));
        try
            hdr = waitLine({'CHUNK:'}, 5);
        catch ME
            err = ME.message;
            return;
        end
        if startsWith(hdr, 'CHUNK:ERR')
            err = hdr;
            return;
        end
        tok = regexp(hdr, '^CHUNK:(\d+) (\d+) ([0-9A-Fa-f]{8})', 'tokens', 'once');
        if isempty(tok) || str2double(tok{1}) ~= off
            err = sprintf('bad reply "%s"', hdr);
            return;
        end
        cnt = str2double(tok{2});
        if cnt == 0
            return;
        end
        raw = read(s, cnt, 'uint8');
        if numel(raw) < cnt
            err = sprintf('only %d of %d bytes came', numel(raw), cnt);
            return;
        end
        d = uint8(raw(:));
        c = crc32Of(d);
        if ~isnan(c) && c ~= hex2dec(tok{3})
            err = sprintf('CRC: DAQ %s, app %08X', upper(tok{3}), c);
            d = uint8([]);
        end
    end

    function c = crc32Of(data)
        % zlib CRC-32 (the same as esp_rom_crc32_le with start value 0). NaN if Java is not available.
        if isempty(data)
            c = 0;
            return;
        end
        try
            b = typecast(data(:)', 'int8');
            j = java.util.zip.CRC32;
            j.update(b, 0, numel(b)); % the (byte[], offset, length) form: always the byte[] method
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
        pnlL = uipanel(filesFig, 'Title', 'Local Files: downloads and live data (select one to plot)', 'FontWeight', 'bold', 'Position', [10, 10, 340, 350]);
        dirField = uieditfield(pnlL, 'text', 'Position', [10, 295, 235, 22], 'Value', downloadDir, ...
            'ValueChangedFcn', @(src, ~) setDownloadDir(src.Value));
        uibutton(pnlL, 'Position', [250, 295, 80, 22], 'Text', 'Browse...', 'ButtonPushedFcn', @browseDir);
        localList = uilistbox(pnlL, 'Position', [10, 45, 320, 240], 'Items', {}, 'ItemsData', {}, 'ValueChangedFcn', @plotSelected);
        uibutton(pnlL, 'Position', [10, 10, 100, 26], 'Text', 'Rescan', 'ButtonPushedFcn', @(~, ~) refreshLocalFiles());
        uibutton(pnlL, 'Position', [120, 10, 100, 26], 'Text', 'Plot Selected', 'ButtonPushedFcn', @plotSelected);
        uibutton(pnlL, 'Position', [230, 10, 100, 26], 'Text', 'Export CSV', 'ButtonPushedFcn', @exportCsv);

        % Plot with start / end range sliders
        axF = uiaxes(filesFig, 'Position', [360, 210, 790, 500]);
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
            'Items', unitItems{1}, 'ValueChangedFcn', @(~, ~) updateRangePlot());
        sensorDropF = uidropdown(filesFig, 'Position', [1030, 85, 120, 22], 'Items', {'PT', 'Load Cell'}, ...
            'ValueChangedFcn', @selectPlotSensor);
        statsLbl = uilabel(filesFig, 'Position', [590, 12, 560, 22], 'Text', '');
        warnLbl = uilabel(filesFig, 'Position', [360, 155, 790, 22], 'Text', '', 'FontWeight', 'bold', ...
            'FontColor', [0.85, 0.33, 0.1]);
        calLbl = uilabel(filesFig, 'Position', [360, 183, 790, 22], 'Text', '');

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
        if xferBusy
            return;
        end
        if isempty(s) || ~isvalid(s)
            uialert(filesFig, 'Connect to a DAQ first (Auto-Connect).', 'Not Connected');
            return;
        end
        try
            pauseSerial();
            boardFiles = queryFiles();
        catch ME
            resumeSerial();
            uialert(filesFig, ME.message, 'File List Error');
            return;
        end
        resumeSerial();
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
        if xferBusy
            return;
        end
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
        dlg = uiprogressdlg(filesFig, 'Title', 'Download', 'Message', 'Stopping the live stream...');
        bad = {};
        notes = {};
        try
            pauseSerial();
        catch ME
            resumeSerial();
            close(dlg);
            uialert(filesFig, ME.message, 'Download Problem');
            return;
        end
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
                [data, ok, msg, retries] = getFile(f, dlg);
            catch ME
                data = uint8([]); ok = false; msg = ME.message; retries = 0;
                pause(0.5);   % the DAQ can still send the rest of a chunk
                flush(s);
            end
            if ok && retries > 0
                notes{end+1} = sprintf('%s: OK, %d chunks were asked for again', localName, retries); %#ok<AGROW>
            end
            if ~isempty(data)
                fid = fopen(fullfile(downloadDir, localName), 'w');
                if fid == -1
                    ok = false; msg = 'cannot write the file in the download folder';
                else
                    fwrite(fid, data, 'uint8');
                    fclose(fid);
                end
            end
            if ~ok
                bad{end+1} = sprintf('%s: %s', localName, msg); %#ok<AGROW>
            end
        end
        resumeSerial();
        close(dlg);
        refreshLocalFiles();
        if ~isempty(bad)
            uialert(filesFig, strjoin([bad, notes], newline), 'Download Problem');
        elseif ~isempty(notes)
            uialert(filesFig, strjoin(notes, newline), 'Download Done (with retries)', 'Icon', 'warning');
        end
    end

    function refreshLocalFiles()
        % The downloaded .BIN files (download folder) and the live data CSV files of the app (output folder)
        if isempty(localList) || ~isvalid(localList)
            return;
        end
        d = dir(fullfile(downloadDir, '*.BIN'));
        L = dir(fullfile(outDir, 'live_data*.csv'));
        [~, order] = sort([L.datenum], 'descend');   % newest live file first
        L = L(order);
        names = [{d.name}, cellfun(@(n) ['[live] ', n], {L.name}, 'UniformOutput', false)];
        paths = [strcat({d.folder}, filesep, {d.name}), strcat({L.folder}, filesep, {L.name})];
        keep = localList.Value;
        localList.Items = names;
        localList.ItemsData = paths;
        if ~isempty(keep) && any(strcmp(paths, keep))
            localList.Value = keep;
        end
    end

    function plotSelected(~, ~)
        fpath = localList.Value;
        if isempty(fpath)
            return;
        end
        [~, base, ext] = fileparts(fpath);
        name = [base, ext];
        if strcmpi(ext, '.csv')
            [sets, nLive] = loadLiveCsv(fpath);
            plotSets = sets; plotBytes = dir(fpath).bytes; plotCal = noCal();
            plotName = name;
            showFileCal();
            fprintf('Loaded %s: %d live samples\n', name, nLive);
            if nLive == 0
                cla(axF);
                title(axF, [name, ' (no samples)'], 'Interpreter', 'none');
                statsLbl.Text = '';
                plotT = [];
                return;
            end
            sensorDropF.Value = sensorDropF.Items{1 + ~isempty(sets(2).t)};
            selectPlotSensor();
            return;
        end
        [plotSets, plotBytes, plotCal] = loadRecords(fpath);
        plotName = name;
        showFileCal();
        nPt = numel(plotSets(1).t);
        nLc = numel(plotSets(2).t);
        fprintf('Loaded %s: %d bytes, %d PT records, %d load cell records\n', name, plotBytes, nPt, nLc);
        if nPt == 0 && nLc == 0
            cla(axF);
            title(axF, [name, ' (no records)'], 'Interpreter', 'none');
            statsLbl.Text = '';
            plotT = [];
            return;
        end
        if numel(plotSets(strcmp(sensorDropF.Items, sensorDropF.Value)).t) == 0
            sensorDropF.Value = sensorDropF.Items{1 + (nPt == 0)}; % the sensor that has data
        end
        selectPlotSensor();
    end

    function exportCsv(~, ~)
        % One CSV with the PT samples, the load cell samples (sorted by time), and the calibration of the file
        fpath = localList.Value;
        if isempty(fpath) || ~endsWith(fpath, '.BIN', 'IgnoreCase', true)
            uialert(filesFig, 'Select a downloaded .BIN file in the list.', 'Nothing Selected');
            return;
        end
        dlg = uiprogressdlg(filesFig, 'Title', 'Export CSV', 'Message', 'Reading and converting...', 'Indeterminate', 'on');
        try
            [sets, ~, cal] = loadRecords(fpath);
            [~, base] = fileparts(fpath);
            outPath = fullfile(fileparts(fpath), [base, '.csv']);
            scale = [0.1875e-3, lcMvVPerCount];
            A = [cal.ptA, cal.lcA]; B = [cal.ptB, cal.lcB];
            isSet = [cal.ptSet, cal.lcSet] & cal.valid;
            blocks = cell(1, 2);
            for k = 1:2
                n = numel(sets(k).t);
                if n == 0, continue; end
                nat = sets(k).raw * scale(k);
                if isSet(k)
                    cv = A(k) .* nat + B(k);
                    a = A(k); b = B(k); rev = cal.rev;
                else
                    cv = nan(n, 1); a = NaN; b = NaN; rev = NaN;
                end
                blocks{k} = [repmat(k, n, 1), sets(k).t(:), sets(k).seq(:), sets(k).raw(:), nat(:), cv(:), ...
                    repmat([a, b, rev], n, 1)];
            end
            M = vertcat(blocks{:});
            if isempty(M)
                close(dlg);
                uialert(filesFig, 'The file has no sample records.', 'Nothing to Export');
                return;
            end
            M = sortrows(M, 2);   % by time
            dlg.Message = sprintf('Writing %d rows...', size(M, 1));
            sensorName = repmat("PT", size(M, 1), 1);
            sensorName(M(:, 1) == 2) = "LC";
            T = table(sensorName, M(:, 2), M(:, 3), M(:, 4), M(:, 5), M(:, 6), M(:, 7), M(:, 8), M(:, 9), 'VariableNames', ...
                {'sensor', 'time_s', 'seq', 'raw_count', 'volts_or_mV_per_V', 'calibrated_PSI_or_N', 'cal_A', 'cal_B', 'cal_revision'});
            writetable(T, outPath);
            close(dlg);
            uialert(filesFig, sprintf('Written %d rows:\n%s', size(M, 1), outPath), 'Export Done', 'Icon', 'success');
        catch ME
            if isvalid(dlg), close(dlg); end
            where = '';
            if ~isempty(ME.stack)
                where = sprintf('\n(in %s, line %d)', ME.stack(1).name, ME.stack(1).line);
            end
            uialert(filesFig, [ME.message, where], 'Export Problem');
        end
    end

    function [sets, n] = loadLiveCsv(fpath)
        % live_data_*.csv: "Time,Voltage" (PT, V) or live_data_loadcell_*.csv: "Time,mV/V". The samples are decimated.
        % The plot code works on raw counts, so the values go back to counts with the scale of the sensor.
        sets = struct('t', {[], []}, 'raw', {[], []}, 'seq', {[], []});
        n = 0;
        fid = fopen(fpath, 'r');
        if fid == -1
            return;
        end
        c = textscan(fid, '%s %f', 'Delimiter', ',', 'HeaderLines', 1);   % a line being written may be cut: textscan stops there
        fclose(fid);
        m = min(numel(c{1}), numel(c{2}));
        if m == 0
            return;
        end
        tm = datetime(c{1}(1:m), 'InputFormat', 'yyyy-MM-dd HH:mm:ss.SSS');
        ok = ~isnat(tm) & isfinite(c{2}(1:m));
        tm = tm(ok); v = c{2}(1:m); v = v(ok);
        n = numel(v);
        if n == 0
            return;
        end
        k = 1 + contains(fpath, 'loadcell');
        scale = [0.1875e-3, lcMvVPerCount];
        sets(k).t = seconds(tm - tm(1));
        sets(k).raw = v(:) / scale(k);
        sets(k).seq = zeros(n, 1);
    end

    function selectPlotSensor(~, ~)
        showFileCal();
        plotSensor = find(strcmp(sensorDropF.Items, sensorDropF.Value), 1);
        unitDrop.Items = unitItems{plotSensor};
        plotT = plotSets(plotSensor).t;
        plotRaw = plotSets(plotSensor).raw;
        plotSeq = plotSets(plotSensor).seq;
        if isempty(plotT)
            cla(axF);
            title(axF, [plotName, ' (no records of this sensor)'], 'Interpreter', 'none');
            statsLbl.Text = '';
            warnLbl.Text = '';
            return;
        end
        tEnd = max(plotT(end), 1e-3);
        startSlider.Limits = [0, tEnd];
        endSlider.Limits = [0, tEnd];
        startSlider.Value = 0;
        endSlider.Value = tEnd;
        updateRangePlot();
    end

    function showFileCal()
        % The calibration that loadRecords found in the loaded file
        if isempty(calLbl) || ~isvalid(calLbl)
            return;
        end
        if plotCal.valid
            calLbl.Text = sprintf('Calibration in the file: %s  (valid blocks: %d)', calText(plotCal, ', '), plotCal.nBlocks);
        else
            calLbl.Text = 'Calibration in the file: none';
        end
    end

    function [sets, nBytes, cal] = loadRecords(fpath)
        % 8-byte records, little endian (firmware record.h). Bit 7 of byte 8 selects the sensor.
        % PT: u32 timestamp_us, i16 reading, 15-bit seq. Load cell: u32 timestamp_us, i24 reading, 7-bit seq.
        % META record: timestamp_us of 0xFFFFFFF0 or more (kind = timestamp_us - 0xFFFFFFF0), bytes 5..8 = payload.
        % It is not a sample: the calibration of the DAQ is in the META records (see calFromMeta).
        sets = struct('t', {[], []}, 'raw', {[], []}, 'seq', {[], []});
        nBytes = 0;
        cal = noCal();
        fid = fopen(fpath, 'r');
        if fid == -1
            return;
        end
        bytes = fread(fid, inf, 'uint8=>uint8');
        fclose(fid);
        nBytes = numel(bytes);
        n = floor(numel(bytes) / 8);
        if n == 0
            return;
        end
        r = reshape(bytes(1:n*8), 8, n);
        ts = double(typecast(reshape(r(1:4, :), [], 1), 'uint32'));

        % Take the META records out first: the sensor decoding below does not see them
        isMeta = ts >= 4294967280; % 0xFFFFFFF0
        if any(isMeta)
            cal = calFromMeta(ts(isMeta) - 4294967280, r(5:8, isMeta), find(isMeta));
            r = r(:, ~isMeta);
            ts = ts(~isMeta);
            n = size(r, 2);
            if n == 0
                return;
            end
        end
        isLc = bitand(r(8, :)', 128) ~= 0;
        % In a file of the firmware before the load cell, that bit is a seq bit: it changes once in 32768 records.
        if sum(diff(isLc) ~= 0) * 16384 < n
            isLc(:) = false;
        end

        pt = ~isLc;
        if any(pt)
            sets(1).raw = double(typecast(reshape(r(5:6, pt), [], 1), 'int16'));
            sets(1).seq = mod(double(typecast(reshape(r(7:8, pt), [], 1), 'uint16')), 32768);
            sets(1).t = secondsOf(ts(pt));
        end
        if any(isLc)
            raw = double(r(5, isLc))' + 256 * double(r(6, isLc))' + 65536 * double(r(7, isLc))';
            raw(raw >= 2^23) = raw(raw >= 2^23) - 2^24;
            sets(2).raw = raw;
            sets(2).seq = double(bitand(r(8, isLc), 127))';
            sets(2).t = secondsOf(ts(isLc));
        end
        if any(pt) && any(isLc)
            % The sensors use one clock: keep their relative time. Time 0 is the first sample of the file.
            firstPt = ts(find(pt, 1));
            firstLc = ts(find(isLc, 1));
            d = mod(firstLc - firstPt + 2^31, 2^32) - 2^31; % us from the first PT sample to the first load cell sample
            sets(1).t = sets(1).t + max(-d, 0) / 1e6;
            sets(2).t = sets(2).t + max(d, 0) / 1e6;
        end
    end

    function cal = calFromMeta(kind, pay, idx)
        % The calibration block: 6 META records in a row with the kinds 1 to 6 (firmware record.h):
        %   1 = flags (bits 31..24: bit 0 PT, bit 1 load cell) and revision (bits 23..0), 2..5 = float32 PT A, PT B, LC A, LC B,
        %   6 = zlib CRC-32 of the 20 payload bytes of the kinds 1 to 5. The file has the block again every 30 s: the last valid one counts.
        % kind: kind of each META record (column), pay: their payload bytes (4 x M, uint8), idx: their place in the file
        cal = noCal();
        M = numel(kind);
        starts = find(kind(1:max(M - 5, 0)) == 1);   % a block begins with kind 1 and needs 5 more records
        for i = starts(:)'
            if ~isequal(kind(i:i+5), (1:6)') || idx(i+5) - idx(i) ~= 5
                continue;   % not in order, or a sample record between them
            end
            body = reshape(pay(:, i:i+4), [], 1);   % the 20 bytes under the CRC
            if crc32Of(body) ~= double(typecast(pay(:, i+5), 'uint32'))
                continue;   % also when crc32Of gives NaN (no Java)
            end
            hd = double(typecast(pay(:, i), 'uint32'));   % flags and revision
            fl = double(typecast(body(5:20), 'single'));   % PT A, PT B, LC A, LC B
            cal.valid = true;
            cal.nBlocks = cal.nBlocks + 1;
            cal.rev = mod(hd, 2^24);
            cal.ptSet = bitand(floor(hd / 2^24), 1) ~= 0;
            cal.lcSet = bitand(floor(hd / 2^24), 2) ~= 0;
            cal.ptA = fl(1); cal.ptB = fl(2); cal.lcA = fl(3); cal.lcB = fl(4);
        end
    end

    function t = secondsOf(ts)
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
            warnLbl.Text = '';
            return;
        end
        if plotSensor == 1
            scale = 0.1875e-3;
            rawName = 'ADS1115 counts';
        else
            scale = lcMvVPerCount;
            rawName = 'NAU7802 counts';
        end
        % A and B: the calibration of the file if it has one for this sensor, else the values of the app
        fileSet = [plotCal.ptSet, plotCal.lcSet];
        fileA = [plotCal.ptA, plotCal.lcA];
        fileB = [plotCal.ptB, plotCal.lcB];
        if plotCal.valid && fileSet(plotSensor) && isfinite(fileA(plotSensor)) && isfinite(fileB(plotSensor))
            fitA = fileA(plotSensor);
            fitB = fileB(plotSensor);
            calSrc = ' [file cal]';
        elseif plotSensor == sensorIdx
            fitA = currentA;
            fitB = currentB;
            calSrc = ' [app cal]';
        else
            fitA = sensorSaved(plotSensor).A;
            fitB = sensorSaved(plotSensor).B;
            calSrc = ' [app cal]';
        end
        switch unitDrop.Value
            case 'Raw counts'
                y = plotRaw; yl = rawName;
            case 'Pressure (PSI, Ax+B)'
                y = fitA .* (plotRaw * scale) + fitB; yl = ['Pressure (PSI)', calSrc];
            case 'Force (N, Ax+B)'
                y = fitA .* (plotRaw * scale) + fitB; yl = ['Force (N)', calSrc];
            otherwise
                y = plotRaw * scale; yl = unitDrop.Value;
        end
        yk = y(k);
        tk = plotT(k);

        % Plot no more than maxPts points. More samples in the range: each bucket of "step" samples gives
        % two points (the min and the max, so the spikes stay visible). A shorter range gives more detail.
        maxPts = 10000;
        minPtsPerSec = 60;
        m = numel(k);
        if m > maxPts
            step = ceil(m / (maxPts / 2));
            nb = ceil(m / step);
            pad = nb * step - m;                       % repeat the last sample: the last bucket is full
            yb = reshape([yk; repmat(yk(end), pad, 1)], step, nb);
            tb = reshape([tk; repmat(tk(end), pad, 1)], step, nb);
            xp = reshape([tb(1, :); tb(end, :)], [], 1);
            yp = reshape([min(yb, [], 1); max(yb, [], 1)], [], 1);
        else
            xp = tk;
            yp = yk;
        end
        span = max(tk(end) - tk(1), 1e-9);
        ptsPerSec = numel(xp) / span;
        if ptsPerSec < minPtsPerSec
            warnLbl.Text = sprintf('Warning: %d of %d samples are plotted (%.0f points/s). Narrow the Start/End range for more detail.', ...
                numel(xp), m, ptsPerSec);
        else
            warnLbl.Text = '';
        end
        cla(axF);
        plot(axF, xp, yp, 'b-');
        xlim(axF, [t0, max(t1, t0 + 1e-6)]);
        title(axF, sprintf('%s  (whole file: %d bytes, %d samples, %.1f s)', plotName, plotBytes, numel(plotT), plotT(end)), 'Interpreter', 'none');
        xlabel(axF, 'Time (s)');
        ylabel(axF, yl);
        grid(axF, 'on');

        ds = mod(diff(plotSeq(k)), 32768 - (plotSensor == 2) * (32768 - 128)); % each sensor has its own seq size
        lost = sum(ds(ds > 1) - 1);
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
        if ~isempty(wifiFig) && isvalid(wifiFig)
            delete(wifiFig);
        end
        closePort();
        if liveDataFile ~= -1
            fclose(liveDataFile);
        end
        if calDataFile ~= -1
            fclose(calDataFile);
        end
        if otherLiveFile ~= -1
            fclose(otherLiveFile);
        end
        if otherCalFile ~= -1
            fclose(otherCalFile);
        end
        if ~isempty(calFig) && isvalid(calFig)
            delete(calFig);
        end
        delete(fig);
    end
end