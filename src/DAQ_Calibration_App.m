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
    yUnit = {'PSI', 'N'};            % the load cell calibration (A, B) is in N: force (N) = A * mV/V + B

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
    parFld = struct();   % the A and B fields of the sensor parameters panel (Files & Plot)
    parEdit = struct('set', [false false], 'A', [NaN NaN], 'B', [NaN NaN]);   % values typed for the loaded file: PT, LC
    plotPath = '';       % the file that is loaded in Files & Plot
    boardFiles = struct('src', {}, 'name', {}, 'bytes', {}, 'start', {});
    plotT = []; plotRaw = []; plotSeq = []; plotName = ''; plotBytes = 0;
    plotSets = struct('t', {[], []}, 'raw', {[], []}, 'seq', {[], []}); % one entry for each sensor of the loaded file
    plotSensor = 1;
    plotCal = noCal(); % the calibration block that loadRecords found in the loaded file
    plotWarn = '';     % warning of the loaded file (damaged records were removed), shown under the plot until another file is loaded
    unitItems = {{'Voltage (V)', 'Pressure (PSI, Ax+B)', 'Raw counts'}, {'Output (mV/V)', 'Force (N, Ax+B)', 'Raw counts'}};

    % Motor performance window (the file of Files & Plot)
    perfFig = []; perfAxT = []; perfAxP = []; perfTxt = []; perfLbl = []; perfRes = [];
    perfFrqBtn = []; perfFrqSld = []; perfFrqLbl = []; perfFrqTxt = []; perfAxFT = []; perfAxFP = [];   % the Frequency tool
    perfFrqMk = gobjects(1, 2);   % the 0.5 s window on the thrust and pressure plots
    perfFrqPos = NaN;             % the centre of the window: s from the burn start
    perfFrqBusy = false;
    lbfToN = 4.4482216152605;        % N per lbf (lbf is the secondary unit in the displays)

    % Handles for performant live plotting and windows
    lineV = [];
    lineP = [];
    calFig = [];           % Handle for the calibration window
    wifiFig = []; wifiF = struct();   % WiFi / Telemetry window and its fields
    wifiPassSet = false;   % the DAQ has a WiFi password (it never sends the password back)
    ntcFig = []; ntcF = struct();     % Temperature (NTC) window and its fields
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
    chainLbl = uilabel(fig, 'Position', [20, 675, 600, 22], 'Text', 'DAQs in chain: (not connected)', 'FontWeight', 'bold');
    btnFiles = uibutton(fig, 'Position', [940, 675, 140, 22], 'Text', 'Files & Plot...', 'ButtonPushedFcn', @openFilesWindow);
    btnNtc = uibutton(fig, 'Position', [640, 675, 140, 22], 'Text', 'Temperature (NTC)...', 'ButtonPushedFcn', @openNtcWindow); %#ok<NASGU>
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
            fprintf(calDataFile, 'mV/V,Force_N\n');
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

        % Reply of NTC GET / NTC MODE / NTC TYPE (and REBOOT, which the NTC window also uses)
        if startsWith(t, 'NTC:') || startsWith(t, 'REBOOT:')
            applyNtcReply(t);
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
            parts{end+1} = sprintf('PT A=%.6g psi/V, B=%.6g psi', c.ptA, c.ptB);
        end
        if c.lcSet
            parts{end+1} = sprintf('LC A=%.6g N per mV/V, B=%.6g N', c.lcA, c.lcB);
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
    % --- Temperature (NTC) window (NTC commands) ---
    % Two thermistors on the ADS1115 pins AIN2 and AIN3 (5V - thermistor - node - 560 ohm - GND). Each is 10k (about room temperature)
    % or 100k (hot parts). The type goes to the flash of the DAQ on USB and into every recorded reading, and it applies at once.
    % The mode applies after the next reboot of that DAQ:
    %   PT (normal)        the normal DAQ: the PT stream, the thermistors are never read
    %   Temperature only   no PT samples, 50 temperature readings/s. The load cell, WiFi, chain, and SD work as before.
    % The firmware sends counts at +/-4.096 V (0.125 mV per count). This window turns them into degrees with the beta equation.
    % =====================================================================
    function openNtcWindow(~, ~)
        if ~isempty(ntcFig) && isvalid(ntcFig)
            figure(ntcFig);
            return;
        end
        ntcFig = uifigure('Name', 'Temperature (NTC) of the connected DAQ', 'Position', [280, 180, 560, 420]);
        ntcF.cfgMsg = '';
        ntcF.readMsg = '';
        y = 375;
        uilabel(ntcFig, 'Position', [20, y, 130, 22], 'Text', 'Mode:');
        ntcF.mode = uidropdown(ntcFig, 'Position', [160, y, 240, 22], 'Items', {'PT (normal)', 'Temperature only (no PT)'}, ...
            'ItemsData', {'PT', 'ONLY'}, 'Value', 'PT');
        y = y - 28;
        uilabel(ntcFig, 'Position', [160, y, 380, 22], 'Text', 'The mode starts after a reboot. Temperature only: no PT, 50 readings/s (+/-4.096 V).', 'FontAngle', 'italic');
        y = y - 40;
        uilabel(ntcFig, 'Position', [20, y, 130, 22], 'Text', 'AIN2 thermistor:');
        ntcF.type2 = uidropdown(ntcFig, 'Position', [160, y, 100, 22], 'Items', {'10k', '100k'}, 'ItemsData', {'10K', '100K'}, 'Value', '10K');
        uilabel(ntcFig, 'Position', [290, y, 80, 22], 'Text', 'Beta (K):');
        ntcF.beta2 = uieditfield(ntcFig, 'numeric', 'Position', [370, y, 80, 22], 'Value', 3950, 'Limits', [1000 10000]);
        y = y - 32;
        uilabel(ntcFig, 'Position', [20, y, 130, 22], 'Text', 'AIN3 thermistor:');
        ntcF.type3 = uidropdown(ntcFig, 'Position', [160, y, 100, 22], 'Items', {'10k', '100k'}, 'ItemsData', {'10K', '100K'}, 'Value', '10K');
        uilabel(ntcFig, 'Position', [290, y, 80, 22], 'Text', 'Beta (K):');
        ntcF.beta3 = uieditfield(ntcFig, 'numeric', 'Position', [370, y, 80, 22], 'Value', 3950, 'Limits', [1000 10000]);
        y = y - 30;
        uilabel(ntcFig, 'Position', [160, y, 380, 22], 'Text', 'The type applies at once. The beta is only for the numbers shown here.', 'FontAngle', 'italic');
        y = y - 45;
        uibutton(ntcFig, 'Position', [20, y, 150, 28], 'Text', 'Read from DAQ', 'ButtonPushedFcn', @ntcRead);
        uibutton(ntcFig, 'Position', [185, y, 150, 28], 'Text', 'Send to DAQ', 'ButtonPushedFcn', @ntcSend);
        uibutton(ntcFig, 'Position', [350, y, 190, 28], 'Text', 'Reboot DAQ (applies the mode)', 'ButtonPushedFcn', @ntcReboot);
        y = y - 20;
        ntcF.status = uilabel(ntcFig, 'Position', [20, 20, 520, y - 20], 'Text', 'Press "Read from DAQ" first.', ...
            'VerticalAlignment', 'top', 'WordWrap', 'on');
        ntcFig.CloseRequestFcn = @(src, ~) delete(src);
    end

    function ok = ntcReady()
        ok = daqReadyForCal();
        if ok && (isempty(ntcFig) || ~isvalid(ntcFig))
            ok = false;
        end
    end

    function ntcRead(~, ~)
        if ~ntcReady(), return; end
        ntcF.status.Text = 'Reading from the DAQ...';
        try
            writeline(s, "NTC GET");
        catch
            ntcF.status.Text = 'Could not send the command to the DAQ.';
        end
    end

    function ntcSend(~, ~)
        if ~ntcReady(), return; end
        cmds = {['NTC TYPE 2 ', ntcF.type2.Value], ['NTC TYPE 3 ', ntcF.type3.Value], ['NTC MODE ', ntcF.mode.Value], 'NTC GET'};
        ntcF.status.Text = 'Sending...';
        try
            for k = 1:numel(cmds)
                writeline(s, string(cmds{k}));
                pause(0.2);          % the DAQ writes each setting to its flash
            end
        catch
            ntcF.status.Text = 'Could not send the commands to the DAQ.';
        end
    end

    function ntcReboot(~, ~)
        if ~ntcReady(), return; end
        sel = uiconfirm(ntcFig, ['The DAQ restarts and starts the saved mode. ', ...
            'It must not be recording. The USB link drops: wait about 5 s, then press Auto-Connect.'], 'Reboot the DAQ', ...
            'Options', {'Reboot', 'Cancel'}, 'DefaultOption', 'Cancel', 'CancelOption', 'Cancel');
        if ~strcmp(sel, 'Reboot'), return; end
        try
            writeline(s, "REBOOT");
            ntcF.status.Text = 'Reboot sent...';
        catch
            ntcF.status.Text = 'Could not send the command to the DAQ.';
        end
    end

    function applyNtcReply(t)
        if isempty(ntcFig) || ~isvalid(ntcFig), return; end
        if startsWith(t, 'REBOOT:')
            if startsWith(t, 'REBOOT:OK')
                ntcF.status.Text = 'The DAQ restarts. Wait about 5 s, then press Auto-Connect.';
            elseif startsWith(t, 'REBOOT:ERR')
                ntcF.status.Text = ['DAQ error: ', strtrim(regexprep(t, '^REBOOT:ERR', ''))];
            end
            return;
        end
        if startsWith(t, 'NTC:ERR')
            ntcF.status.Text = ['DAQ error: ', strtrim(regexprep(t, '^NTC:ERR', ''))];
            return;
        end
        tok = regexp(t, '^NTC:CFG\s+mode=(\w+)\s+saved=(\w+)\s+type2=(\w+)\s+type3=(\w+)', 'tokens', 'once');
        if ~isempty(tok)
            ntcF.mode.Value = tok{2};
            ntcF.type2.Value = tok{3};
            ntcF.type3.Value = tok{4};
            names = struct('PT', 'PT (normal)', 'ONLY', 'Temperature only');
            ntcF.cfgMsg = sprintf('Running now: %s. Saved: %s.', names.(tok{1}), names.(tok{2}));
            if ~strcmp(tok{1}, tok{2})
                ntcF.cfgMsg = [ntcF.cfgMsg, ' The saved mode starts after a reboot.'];
            end
        elseif startsWith(t, 'NTC:NONE')
            ntcF.readMsg = 'No reading yet: the DAQ is in PT mode, or no thermistor is installed (voltage below the 0 degC limit).';
        else
            tk = regexp(t, '^NTC:OK\s+age_ms=(\d+)\s+a2=(\S+)\s+a3=(\S+)\s+scans=(\d+)\s+failed=(\d+)', 'tokens', 'once');
            if isempty(tk), return; end
            ntcF.readMsg = sprintf('AIN2: %s\nAIN3: %s\nReading age %s ms. Scans %s, failed %s.', ...
                ntcText(tk{2}, ntcF.beta2.Value), ntcText(tk{3}, ntcF.beta3.Value), tk{1}, tk{4}, tk{5});
        end
        ntcF.status.Text = strtrim(sprintf('%s\n%s', ntcF.cfgMsg, ntcF.readMsg));
    end

    function txt = ntcText(f, beta)
        % f = "<count>/<10K|100K>/<mV>" or "-" (not installed). Count at +/-4.096 V: 0.125 mV. Node = 5 V x 560 / (R + 560).
        if strcmp(f, '-')
            txt = 'not installed';
            return;
        end
        p = split(string(f), '/');
        count = str2double(p(1));
        r25 = 10e3;
        if p(2) == "100K", r25 = 100e3; end
        v = count * 0.125e-3;
        if v <= 0 || v >= 5
            temp = NaN;
        else
            r = 560 * (5 / v - 1);
            temp = 1 / (1 / 298.15 + log(r / r25) / beta) - 273.15;
        end
        txt = sprintf('%d counts, %.1f mV, %s thermistor: %.1f degC', round(count), v * 1e3, lower(char(p(2))), temp);
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
        c = crc32Of(d);   % never NaN: without Java, crc32Of uses a table loop. A chunk is never accepted without a check.
        if c ~= hex2dec(tok{3})
            err = sprintf('CRC: DAQ %s, app %08X', upper(tok{3}), c);
            d = uint8([]);
        end
    end

    function c = crc32Of(data)
        % zlib CRC-32 (the same as esp_rom_crc32_le with start value 0). Java when it works (fast, checked once against a known
        % value), else a table loop in plain MATLAB (slower: about 10 ms for a 16 KB chunk). The result is never NaN.
        persistent useJava tbl
        if isempty(useJava)
            useJava = false;
            try
                j = java.util.zip.CRC32;
                j.update(int8([1 2 3]), 0, 3);
                useJava = j.getValue() == 1438416925;   % CRC-32 of the bytes 01 02 03
            catch
            end
            tbl = zeros(1, 256, 'uint32');
            for i = 0:255
                v = uint32(i);
                for k = 1:8
                    if bitand(v, uint32(1))
                        v = bitxor(uint32(3988292384), bitshift(v, -1));   % 0xEDB88320
                    else
                        v = bitshift(v, -1);
                    end
                end
                tbl(i + 1) = v;
            end
        end
        if isempty(data)
            c = 0;
            return;
        end
        data = reshape(uint8(data), 1, []);
        if useJava
            b = typecast(data, 'int8');
            j = java.util.zip.CRC32;
            j.update(b, 0, numel(b)); % the (byte[], offset, length) form: always the byte[] method
            c = double(j.getValue());
        else
            v = uint32(4294967295);
            for i = 1:numel(data)
                v = bitxor(tbl(bitand(bitxor(v, uint32(data(i))), uint32(255)) + 1), bitshift(v, -8));
            end
            c = double(bitxor(v, uint32(4294967295)));
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
        filesFig = uifigure('Name', 'DAQ Files & Plot', 'Position', [150, 40, 1160, 800]);

        % Files on the connected DAQ (NAND = PFS, micro SD = SD)
        pnlB = uipanel(filesFig, 'Title', 'Files on the DAQ (NAND / micro SD)', 'FontWeight', 'bold', 'Position', [10, 450, 340, 340]);
        filesInfoLbl = uilabel(pnlB, 'Position', [10, 290, 320, 22], 'Text', 'Connected DAQ: none');
        boardList = uilistbox(pnlB, 'Position', [10, 45, 320, 240], 'Items', {}, 'Multiselect', 'on');
        uibutton(pnlB, 'Position', [10, 10, 100, 26], 'Text', 'Refresh List', 'ButtonPushedFcn', @refreshBoardFiles);
        uibutton(pnlB, 'Position', [120, 10, 210, 26], 'Text', 'Download Selected', 'ButtonPushedFcn', @downloadSelected);

        % Downloaded files in the local folder
        pnlL = uipanel(filesFig, 'Title', 'Local Files: downloads and live data (select one to plot)', 'FontWeight', 'bold', 'Position', [10, 10, 340, 430]);
        dirField = uieditfield(pnlL, 'text', 'Position', [10, 375, 235, 22], 'Value', downloadDir, ...
            'ValueChangedFcn', @(src, ~) setDownloadDir(src.Value));
        uibutton(pnlL, 'Position', [250, 375, 80, 22], 'Text', 'Browse...', 'ButtonPushedFcn', @browseDir);
        localList = uilistbox(pnlL, 'Position', [10, 112, 320, 253], 'Items', {}, 'ItemsData', {}, 'ValueChangedFcn', @plotSelected);
        uibutton(pnlL, 'Position', [10, 78, 320, 26], 'Text', 'Motor performance...', 'FontWeight', 'bold', ...
            'ButtonPushedFcn', @openPerf);
        uibutton(pnlL, 'Position', [10, 44, 155, 26], 'Text', 'Export CSV', 'ButtonPushedFcn', @exportCsv, ...
            'Tooltip', 'CSV of the whole loaded file: asks for the format (individual, combined 1 kHz, debug)');
        uibutton(pnlL, 'Position', [175, 44, 155, 26], 'Text', 'Export range CSV', 'ButtonPushedFcn', @exportRangeCsv, ...
            'Tooltip', 'CSV of the samples between the Start and End sliders: asks for the format');
        uibutton(pnlL, 'Position', [10, 10, 155, 26], 'Text', 'Rescan', 'ButtonPushedFcn', @(~, ~) refreshLocalFiles());
        uibutton(pnlL, 'Position', [175, 10, 155, 26], 'Text', 'Plot Selected', 'ButtonPushedFcn', @plotSelected);

        % Plot with start / end range sliders
        axF = uiaxes(filesFig, 'Position', [360, 210, 790, 470]);
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
        makeParPanel();

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
        loadSelected();
        if ~isempty(perfFig) && isvalid(perfFig)
            runPerf();   % the motor performance window follows the loaded file
        end
    end

    function loadSelected()
        fpath = localList.Value;
        if isempty(fpath)
            return;
        end
        [~, base, ext] = fileparts(fpath);
        name = [base, ext];
        plotPath = fpath;
        parEdit = struct('set', [false false], 'A', [NaN NaN], 'B', [NaN NaN]);   % typed values belong to one file
        if strcmpi(ext, '.csv')
            [sets, nLive] = loadLiveCsv(fpath);
            plotSets = sets; plotBytes = dir(fpath).bytes; plotCal = noCal();
            plotName = name;
            plotWarn = '';
            showFileCal();
            refreshParFields();
            fprintf('Loaded %s: %d live samples\n', name, nLive);
            if nLive == 0
                cla(axF);
                title(axF, [name, ' (no samples)'], 'Interpreter', 'none');
                statsLbl.Text = '';
                setWarn('');
                plotT = [];
                return;
            end
            sensorDropF.Value = sensorDropF.Items{1 + ~isempty(sets(2).t)};
            applySensor(true);
            return;
        end
        [plotSets, plotBytes, plotCal, plotBad] = loadRecords(fpath);
        plotName = name;
        plotWarn = '';
        if plotBad.pt + plotBad.lc > 0
            plotWarn = sprintf('WARNING: damaged file, %d records with a wrong time were removed (PT %d, LC %d). Download it again.', ...
                plotBad.pt + plotBad.lc, plotBad.pt, plotBad.lc);
        end
        showFileCal();
        refreshParFields();
        nPt = numel(plotSets(1).t);
        nLc = numel(plotSets(2).t);
        fprintf('Loaded %s: %d bytes, %d PT records, %d load cell records\n', name, plotBytes, nPt, nLc);
        if nPt == 0 && nLc == 0
            cla(axF);
            title(axF, [name, ' (no records)'], 'Interpreter', 'none');
            statsLbl.Text = '';
            setWarn('');
            plotT = [];
            return;
        end
        if numel(plotSets(strcmp(sensorDropF.Items, sensorDropF.Value)).t) == 0
            sensorDropF.Value = sensorDropF.Items{1 + (nPt == 0)}; % the sensor that has data
        end
        applySensor(true);
        if plotBad.pt + plotBad.lc > 0
            fprintf('%s: %s\n', name, plotWarn);
            uialert(filesFig, sprintf(['%d records of %s have a wrong time (PT %d, LC %d). They are left out of the plot, ', ...
                'the CSV files and the motor performance, so the time axis is right, but data is missing at those places.\n\n', ...
                'A common cause is a bad download: a USB packet was lost and the CRC check did not catch it. ', ...
                'Download the file again from the DAQ, or use the same data from another copy (NAND or uSD).'], ...
                plotBad.pt + plotBad.lc, name, plotBad.pt, plotBad.lc), 'Damaged File', 'Icon', 'warning');
        end
    end

    function exportCsv(~, ~)
        exportData(false);
    end

    function exportRangeCsv(~, ~)
        exportData(true);
    end

    function exportData(trim)
        % Asks for the format, then writes the CSV file(s) of the loaded .BIN file: all the samples, or (trim = true) only the samples
        % between the Start and End sliders (time_s keeps the time of the whole file). The calibrated values use the sensor
        % parameters of this window (edited, file cal or app cal).
        if isempty(plotPath) || ~endsWith(plotPath, '.BIN', 'IgnoreCase', true) || (isempty(plotSets(1).t) && isempty(plotSets(2).t))
            uialert(filesFig, 'Select a downloaded .BIN file in the list.', 'Nothing Selected');
            return;
        end
        msg = sprintf(['Choose the CSV format:\n\n', ...
            'Individual: one CSV for each sensor (PT and load cell) with the time, the pressure (psi) or the force (N), ', ...
            'the raw count and the calibration parameters.\n\n', ...
            'Combined 1 kHz: one CSV with the time, the pressure (psi) and the thrust (N). Both sensors are interpolated to 1 kHz ', ...
            '(modified Akima cubic, makima). No raw counts and no calibration parameters.\n\n', ...
            'Debug: the old format, one row for each sample of both sensors, with the raw count and the calibration.']);
        choice = uiconfirm(filesFig, msg, 'CSV export', 'Options', {'Individual', 'Combined 1 kHz', 'Debug', 'Cancel'}, ...
            'DefaultOption', 2, 'CancelOption', 4, 'Icon', 'question');
        if strcmp(choice, 'Cancel')
            return;
        end
        if trim
            t0 = startSlider.Value; t1 = endSlider.Value;
        else
            t0 = -Inf; t1 = Inf;
        end
        [folder, base] = fileparts(plotPath);
        stem = base;
        if trim
            stem = sprintf('%s_trim_%.3f_to_%.3f_s', base, t0, t1);
        end
        dlg = uiprogressdlg(filesFig, 'Title', 'Export CSV', 'Message', 'Converting...', 'Indeterminate', 'on');
        try
            switch choice
                case 'Individual'
                    info = exportIndividual(fullfile(folder, stem), t0, t1);
                case 'Combined 1 kHz'
                    info = exportCombined(fullfile(folder, [stem, '_combined_1kHz.csv']), t0, t1);
                otherwise
                    info = exportDebug(fullfile(folder, [stem, '_debug.csv']), t0, t1);
            end
            close(dlg);
            uialert(filesFig, info, 'Export Done', 'Icon', 'success');
        catch ME
            if isvalid(dlg), close(dlg); end
            if strcmp(ME.identifier, 'DAQ:nothing')
                uialert(filesFig, ME.message, 'Nothing to Export');
                return;
            end
            where = '';
            if ~isempty(ME.stack)
                where = sprintf('\n(in %s, line %d)', ME.stack(1).name, ME.stack(1).line);
            end
            uialert(filesFig, [ME.message, where], 'Export Problem');
        end
    end

    function txt = parSummary()
        % The sensor parameters that an export used, for the message after it
        [aP, bP, sP] = sensorParams(1);
        [aL, bL, sL] = sensorParams(2);
        txt = sprintf('PT: A = %.7g psi/V, B = %.7g psi (%s)\nLC: A = %.7g N per mV/V, B = %.7g N (%s)', aP, bP, sP, aL, bL, sL);
    end

    function info = exportIndividual(stemPath, t0, t1)
        % One CSV for each sensor that has samples: <stem>_PT.csv and <stem>_LC.csv
        tag = {'PT', 'LC'};
        scale = [0.1875e-3, lcMvVPerCount];
        varNames = {{'time_s', 'pressure_psi', 'raw_count', 'cal_A_psi_per_V', 'cal_B_psi', 'cal_source', 'cal_revision'}, ...
                    {'time_s', 'force_N', 'raw_count', 'cal_A_N_per_mV_per_V', 'cal_B_N', 'cal_source', 'cal_revision'}};
        written = {};
        for k = 1:2
            tk = plotSets(k).t(:);
            sel = tk >= t0 & tk <= t1;
            n = nnz(sel);
            if n == 0, continue; end
            raw = plotSets(k).raw(:); raw = raw(sel);
            [a, b, sName] = sensorParams(k);
            if strcmp(sName, 'file cal'), rev = plotCal.rev; else, rev = NaN; end
            T = table(tk(sel), a * raw * scale(k) + b, raw, repmat(a, n, 1), repmat(b, n, 1), ...
                repmat(string(strrep(sName, ' cal', '')), n, 1), repmat(rev, n, 1), 'VariableNames', varNames{k});
            f = [stemPath, '_', tag{k}, '.csv'];
            writetable(T, f);
            written{end+1} = sprintf('%s  (%d rows)', f, n);   %#ok<AGROW>
        end
        if isempty(written)
            error('DAQ:nothing', 'The file has no samples in this range.');
        end
        info = sprintf('Written:\n%s\n\nSensor parameters used:\n%s', strjoin(written, newline), parSummary());
    end

    function info = exportCombined(outPath, t0, t1)
        % time_s, pressure_psi, thrust_N on a 1 kHz grid. Each sensor is interpolated with makima (resampleGaps). The grid covers the time
        % that both sensors have (the one that has samples, when the other has none). A gap in the data and the time outside the data of
        % a sensor stay empty (NaN): nothing is invented there.
        scale = [0.1875e-3, lcMvVPerCount];
        ser = cell(1, 2);
        span = zeros(0, 2);
        for k = 1:2
            tk = plotSets(k).t(:);
            sel = tk >= t0 & tk <= t1;
            if ~any(sel), continue; end
            raw = plotSets(k).raw(:);
            [a, b] = sensorParams(k);
            ser{k} = {tk(sel), a * raw(sel) * scale(k) + b};
            span(end+1, :) = [tk(find(sel, 1)), tk(find(sel, 1, 'last'))];   %#ok<AGROW>
        end
        if isempty(span)
            error('DAQ:nothing', 'The file has no samples in this range.');
        end
        lo = max(span(:, 1));
        hi = min(span(:, 2));
        k0 = ceil(lo * 1000);
        k1 = floor(hi * 1000);
        if k1 <= k0
            error('DAQ:nothing', 'The two sensors have no common time in this range.');
        end
        tg = (k0:k1)' / 1000;                               % the grid in whole milliseconds
        P = nan(size(tg));
        F = nan(size(tg));
        if ~isempty(ser{1}), P = resampleGaps(ser{1}{1}, ser{1}{2}, tg); end
        if ~isempty(ser{2}), F = resampleGaps(ser{2}{1}, ser{2}{2}, tg); end
        fid = fopen(outPath, 'w');
        if fid == -1
            error('Cannot write %s.', outPath);
        end
        fprintf(fid, 'time_s,pressure_psi,thrust_N\n');
        fprintf(fid, '%.3f,%.4f,%.4f\n', [tg, P, F]');
        fclose(fid);
        info = sprintf('Written %d rows at 1 kHz (%.3f to %.3f s):\n%s\n\nSensor parameters used:\n%s', numel(tg), tg(1), tg(end), outPath, parSummary());
        if all(isnan(P)), info = [info, sprintf('\n\nThe file has no PT samples in this range: the pressure column is empty (NaN).')]; end
        if all(isnan(F)), info = [info, sprintf('\n\nThe file has no load cell samples in this range: the thrust column is empty (NaN).')]; end
        nGap = nnz(isnan(P) & ~all(isnan(P))) + nnz(isnan(F) & ~all(isnan(F)));
        if nGap > 0
            info = [info, sprintf('\n\n%d values are empty (NaN) where the sensor has a data gap.', nGap)];
        end
    end

    function info = exportDebug(outPath, t0, t1)
        % The old format: one row for each sample of both sensors (sorted by time), with the raw count and the calibration
        scale = [0.1875e-3, lcMvVPerCount];
        blocks = cell(1, 2);
        srcs = {strings(0, 1), strings(0, 1)};              % the cal_source of each row
        for k = 1:2
            tk = plotSets(k).t(:);
            sel = tk >= t0 & tk <= t1;
            n = nnz(sel);
            if n == 0, continue; end
            [a, b, sName] = sensorParams(k);
            raw = plotSets(k).raw(:); raw = raw(sel);
            sq = plotSets(k).seq(:); sq = sq(sel);
            nat = raw * scale(k);                           % V (PT) or mV/V (load cell)
            if strcmp(sName, 'file cal'), rev = plotCal.rev; else, rev = NaN; end
            blocks{k} = [repmat(k, n, 1), tk(sel), sq, raw, nat, a .* nat + b, repmat([a, b, rev], n, 1)];
            srcs{k} = repmat(string(strrep(sName, ' cal', '')), n, 1);
        end
        M = vertcat(blocks{:});
        S = vertcat(srcs{:});
        if isempty(M)
            error('DAQ:nothing', 'The file has no samples in this range.');
        end
        [M, ord] = sortrows(M, 2);                          % by time
        S = S(ord);
        sensorName = repmat("PT", size(M, 1), 1);
        sensorName(M(:, 1) == 2) = "LC";
        T = table(sensorName, M(:, 2), M(:, 3), M(:, 4), M(:, 5), M(:, 6), M(:, 7), M(:, 8), S, M(:, 9), 'VariableNames', ...
            {'sensor', 'time_s', 'seq', 'raw_count', 'volts_or_mV_per_V', 'calibrated_PSI_or_N', 'cal_A', 'cal_B', 'cal_source', 'cal_revision'});
        writetable(T, outPath);
        info = sprintf('Written %d rows:\n%s\n\nSensor parameters used:\n%s', size(M, 1), outPath, parSummary());
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
        applySensor(false);   % the sensor list: the Start/End range stays
    end

    function applySensor(resetRange)
        % Shows the sensor of the list. resetRange: a new file (Start/End go to the whole file). The sliders cover the time of both
        % sensors (one time base), so changing the sensor does not move them.
        showFileCal();
        plotSensor = find(strcmp(sensorDropF.Items, sensorDropF.Value), 1);
        ui = find(strcmp(unitDrop.Items, unitDrop.Value), 1);   % the same kind of unit: voltage/output, pressure/force, raw
        if isempty(ui), ui = 1; end
        unitDrop.Items = unitItems{plotSensor};
        unitDrop.Value = unitDrop.Items{ui};
        plotT = plotSets(plotSensor).t;
        plotRaw = plotSets(plotSensor).raw;
        plotSeq = plotSets(plotSensor).seq;
        tEnd = 1e-3;
        for q = 1:2
            if ~isempty(plotSets(q).t), tEnd = max(tEnd, plotSets(q).t(end)); end
        end
        if resetRange || ~isequal(startSlider.Limits, [0, tEnd])
            startSlider.Limits = [0, tEnd];
            endSlider.Limits = [0, tEnd];
            startSlider.Value = 0;
            endSlider.Value = tEnd;
        end
        if isempty(plotT)
            cla(axF);
            title(axF, [plotName, ' (no records of this sensor)'], 'Interpreter', 'none');
            statsLbl.Text = '';
            setWarn('');
            return;
        end
        updateRangePlot();
    end

    function setWarn(extra)
        % The line under the plot: the warning of the loaded file (damaged records) and, after it, the warning about the plot.
        if isempty(warnLbl) || ~isvalid(warnLbl)
            return;
        end
        parts = {plotWarn, extra};
        warnLbl.Text = strjoin(parts(~cellfun(@isempty, parts)), '   ');
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

    function [sets, nBytes, cal, bad] = loadRecords(fpath)
        % 8-byte records, little endian (firmware record.h). Bit 7 of byte 8 selects the sensor.
        % PT: u32 timestamp_us, i16 reading, 15-bit seq. Load cell: u32 timestamp_us, i24 reading, 7-bit seq.
        % META record: timestamp_us of 0xFFFFFFF0 or more (kind = timestamp_us - 0xFFFFFFF0), bytes 5..8 = payload.
        % It is not a sample: the calibration of the DAQ is in the META records (see calFromMeta).
        % bad.pt, bad.lc: records that were removed because their time is wrong (see badTimeMask): a damaged file
        sets = struct('t', {[], []}, 'raw', {[], []}, 'seq', {[], []});
        nBytes = 0;
        cal = noCal();
        bad = struct('pt', 0, 'lc', 0);
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
        [pt, bad.pt] = dropBadTimes(pt, ts);   % a damaged file has records with a wrong time: they would stretch the time axis
        [isLc, bad.lc] = dropBadTimes(isLc, ts);
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

    function [cal, blkIdx] = calFromMeta(kind, pay, idx)
        % The calibration block: 6 META records in a row with the kinds 1 to 6 (firmware record.h):
        %   1 = flags (bits 31..24: bit 0 PT, bit 1 load cell) and revision (bits 23..0), 2..5 = float32 PT A, PT B, LC A, LC B,
        %   6 = zlib CRC-32 of the 20 payload bytes of the kinds 1 to 5. The file has the block again every 30 s: the last valid one counts.
        % kind: kind of each META record (column), pay: their payload bytes (4 x M, uint8), idx: their place in the file
        cal = noCal();
        blkIdx = [];   % the place (record number) of the first record of each valid block
        M = numel(kind);
        starts = find(kind(1:max(M - 5, 0)) == 1);   % a block begins with kind 1 and needs 5 more records
        for i = starts(:)'
            if ~isequal(kind(i:i+5), (1:6)') || idx(i+5) - idx(i) ~= 5
                continue;   % not in order, or a sample record between them
            end
            body = reshape(pay(:, i:i+4), [], 1);   % the 20 bytes under the CRC
            if crc32Of(body) ~= double(typecast(pay(:, i+5), 'uint32'))
                continue;
            end
            hd = double(typecast(pay(:, i), 'uint32'));   % flags and revision
            fl = double(typecast(body(5:20), 'single'));   % PT A, PT B, LC A, LC B
            cal.valid = true;
            cal.nBlocks = cal.nBlocks + 1;
            blkIdx(end+1) = idx(i);   %#ok<AGROW>
            cal.rev = mod(hd, 2^24);
            cal.ptSet = bitand(floor(hd / 2^24), 1) ~= 0;
            cal.lcSet = bitand(floor(hd / 2^24), 2) ~= 0;
            cal.ptA = fl(1); cal.ptB = fl(2); cal.lcA = fl(3); cal.lcB = fl(4);
        end
    end

    function [m, nBad] = dropBadTimes(m, ts)
        % m: the records of one sensor (logical, in file order). Clear the ones that badTimeMask finds. ts: the times of all records.
        i = find(m);
        nBad = 0;
        if numel(i) < 3
            return;
        end
        [k, nBad] = badTimeMask(ts(i));
        m(i(~k)) = false;
    end

    function [keep, nBad] = badTimeMask(ts)
        % ts: the times (us, uint32 as double) of the records of one sensor, in file order. keep: false for a record with a wrong time.
        % The time of one sensor only goes forward (the wrap of the 32-bit clock is a small step forward, modulo 2^32).
        % Pass 1: a step that is not small cuts the records in segments. A segment of fewer than minSeg records is damage
        %   (for example log text that a bad download put in the file). A long segment is data, also after a pause of the recording.
        % Pass 2: a step back in time. The record that is out of order is removed. A download that lost a piece of the data
        %   joins the first bytes of one record to the last bytes of another: its time is wrong by up to 65 ms.
        % Without this, every step back adds 2^32 us (71 min) to the time axis. Copy in tools/time_clean_mirror.py: change both.
        minSeg = 50;
        n = numel(ts);
        keep = true(n, 1);
        nBad = 0;
        if n < 3
            return;
        end
        d = mod(diff(ts), 2^32);                               % the step forward, with the wrap of the clock
        tol = max(1e5, 500 * median(d));
        cut = find(d > tol);
        if ~isempty(cut)
            segStart = [1; cut + 1];
            segEnd = [cut; n];
            short = find(segEnd - segStart + 1 < minSeg);
            if numel(short) < numel(segStart)                  % when all are short, there is no long segment to compare with
                for i = short(:)'
                    keep(segStart(i):segEnd(i)) = false;
                end
            end
        end
        for pass = 1:50
            idx = find(keep);
            stp = mod(diff(ts(idx)) + 2^31, 2^32) - 2^31;      % the step with its sign
            back = find(stp < 0);                                % a step back, between the kept records back and back + 1
            if isempty(back)
                break;
            end
            u = [0; cumsum(stp)];
            drop = back + 1;                                   % the later record is the bad one ...
            late = false(size(back));
            ok = back > 1;
            late(ok) = u(back(ok) + 1) >= u(back(ok) - 1);
            drop(late) = back(late);                           % ... unless the next one still follows the one before
            keep(idx(unique(drop))) = false;
        end
        nBad = sum(~keep);
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
        if isempty(axF) || ~isvalid(axF)
            return;
        end
        refreshParFields();
        if isempty(plotT)
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
            setWarn('');
            return;
        end
        if plotSensor == 1
            scale = 0.1875e-3;
            rawName = 'ADS1115 counts';
        else
            scale = lcMvVPerCount;
            rawName = 'NAU7802 counts';
        end
        % A and B: the values typed in this window, else the calibration of the file, else the values of the app
        [fitA, fitB, srcName] = sensorParams(plotSensor);
        calSrc = [' [', srcName, ']'];
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
            setWarn(sprintf('Warning: %d of %d samples are plotted (%.0f points/s). Narrow the Start/End range for more detail.', ...
                numel(xp), m, ptsPerSec));
        else
            setWarn('');
        end
        cla(axF);
        plot(axF, xp, yp, 'b-');
        xlim(axF, [t0, max(t1, t0 + 1e-6)]);
        % Y: fitted to the points that are plotted (the minimum and the maximum of every bucket are in them, so every sample of the range
        % fits), with 5 % space. It is set on every update, so a zoom or a pan with the axes toolbar (which turns the automatic
        % mode off for good) cannot leave the old limits when the range, the unit or the file changes.
        fin = yp(isfinite(yp));
        if ~isempty(fin)
            yLo = min(fin); yHi = max(fin);
            yPad = 0.05 * (yHi - yLo);
            if yPad == 0, yPad = max(0.05 * abs(yHi), 1e-6); end   % a flat line
            ylim(axF, [yLo - yPad, yHi + yPad]);
        end
        title(axF, sprintf('%s  (whole file: %d bytes, %d samples, %.1f s)', plotName, plotBytes, numel(plotT), plotT(end)), 'Interpreter', 'none');
        xlabel(axF, 'Time (s)');
        ylabel(axF, yl);
        grid(axF, 'on');

        ds = mod(diff(plotSeq(k)), 32768 - (plotSensor == 2) * (32768 - 128)); % each sensor has its own seq size
        lost = sum(ds(ds > 1) - 1);
        statsLbl.Text = sprintf('%d samples  %.3f s   mean %.4f  min %.4f  max %.4f  std %.4f   lost samples %d', ...
            m, tk(end) - tk(1), mean(yk), min(yk), max(yk), std(yk), lost);
        if strcmp(unitDrop.Value, 'Force (N, Ax+B)')
            statsLbl.Text = [statsLbl.Text, sprintf('   (max %.2f lbf)', max(yk) / lbfToN)];   % lbf beside N
        end
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

    % =====================================================================
    % --- Sensor parameters of the loaded file (A and B) ---
    % =====================================================================
    % The plot, the CSV exports and the motor performance use ONE set of parameters for each sensor. The first that exists wins:
    %   edited     the values typed in the fields of the panel (for the loaded file only: a new file clears them)
    %   file cal   the last valid calibration block inside the file
    %   app cal    the values of the calibration view of this app
    % The raw samples are never changed. "Write to .BIN file..." puts the values into the calibration blocks of the file (it asks first).
    % PT: pressure (PSI) = A * V + B. Load cell: force (N) = A * mV/V + B.
    function [A, B, srcName] = sensorParams(k)
        if parEdit.set(k)
            A = parEdit.A(k); B = parEdit.B(k); srcName = 'edited';
            return;
        end
        fileSet = [plotCal.ptSet, plotCal.lcSet];
        fileA = [plotCal.ptA, plotCal.lcA];
        fileB = [plotCal.ptB, plotCal.lcB];
        if plotCal.valid && fileSet(k) && isfinite(fileA(k)) && isfinite(fileB(k))
            A = fileA(k); B = fileB(k); srcName = 'file cal';
        elseif k == sensorIdx
            A = currentA; B = currentB; srcName = 'app cal';
        else
            A = sensorSaved(k).A; B = sensorSaved(k).B; srcName = 'app cal';
        end
    end

    function makeParPanel()
        pnlP = uipanel(filesFig, 'Title', 'Sensor parameters of this file (the plot, the exports and the motor performance use them)', ...
            'FontWeight', 'bold', 'Position', [360, 690, 790, 100]);
        uilabel(pnlP, 'Position', [10, 44, 30, 22], 'Text', 'PT', 'FontWeight', 'bold');
        uilabel(pnlP, 'Position', [40, 44, 100, 22], 'Text', 'A (PSI per V)');
        parFld.ptA = uieditfield(pnlP, 'numeric', 'Position', [140, 44, 95, 22], 'ValueDisplayFormat', '%.7g', ...
            'ValueChangedFcn', @(~, ~) parEdited(1));
        uilabel(pnlP, 'Position', [245, 44, 60, 22], 'Text', 'B (PSI)');
        parFld.ptB = uieditfield(pnlP, 'numeric', 'Position', [305, 44, 95, 22], 'ValueDisplayFormat', '%.7g', ...
            'ValueChangedFcn', @(~, ~) parEdited(1));
        parFld.ptSrc = uilabel(pnlP, 'Position', [410, 44, 150, 22], 'Text', '');
        uilabel(pnlP, 'Position', [10, 12, 30, 22], 'Text', 'LC', 'FontWeight', 'bold');
        uilabel(pnlP, 'Position', [40, 12, 100, 22], 'Text', 'A (N per mV/V)');
        parFld.lcA = uieditfield(pnlP, 'numeric', 'Position', [140, 12, 95, 22], 'ValueDisplayFormat', '%.7g', ...
            'ValueChangedFcn', @(~, ~) parEdited(2));
        uilabel(pnlP, 'Position', [245, 12, 60, 22], 'Text', 'B (N)');
        parFld.lcB = uieditfield(pnlP, 'numeric', 'Position', [305, 12, 95, 22], 'ValueDisplayFormat', '%.7g', ...
            'ValueChangedFcn', @(~, ~) parEdited(2));
        parFld.lcSrc = uilabel(pnlP, 'Position', [410, 12, 150, 22], 'Text', '');
        uibutton(pnlP, 'Position', [570, 44, 100, 22], 'Text', 'File values', 'ButtonPushedFcn', @(~, ~) parReset(), ...
            'Tooltip', 'Forget the typed values: use the calibration of the file');
        uibutton(pnlP, 'Position', [680, 44, 100, 22], 'Text', 'Monitor scales', 'ButtonPushedFcn', @(~, ~) parMonitor(), ...
            'Tooltip', 'The scales of DAQ_Live_Monitor on this PC (B = 0)');
        uibutton(pnlP, 'Position', [570, 12, 210, 22], 'Text', 'Write to .BIN file...', 'ButtonPushedFcn', @(~, ~) writeCalToFile(), ...
            'Tooltip', 'Put these values into the calibration blocks of the file (asks first)');
        refreshParFields();
    end

    function refreshParFields()
        % The fields always show the values that the plot uses
        if ~isfield(parFld, 'ptA') || ~isvalid(parFld.ptA)
            return;
        end
        fa = {parFld.ptA, parFld.lcA}; fb = {parFld.ptB, parFld.lcB}; fl = {parFld.ptSrc, parFld.lcSrc};
        for q = 1:2
            [a, b, sn] = sensorParams(q);
            if ~isfinite(a), a = 0; end
            if ~isfinite(b), b = 0; end
            fa{q}.Value = a;
            fb{q}.Value = b;
            if strcmp(sn, 'file cal')
                fl{q}.Text = sprintf('[file cal, rev %d]', plotCal.rev);
                fl{q}.FontColor = [0, 0, 0];
            elseif strcmp(sn, 'edited')
                fl{q}.Text = '[edited]';
                fl{q}.FontColor = [0.85, 0.33, 0.1];
            else
                fl{q}.Text = '[app cal: not in the file]';
                fl{q}.FontColor = [0.5, 0.5, 0.5];
            end
        end
    end

    function parEdited(k)
        if k == 1
            fa = parFld.ptA; fb = parFld.ptB;
        else
            fa = parFld.lcA; fb = parFld.lcB;
        end
        parEdit.set(k) = true;
        parEdit.A(k) = fa.Value;
        parEdit.B(k) = fb.Value;
        parChanged();
    end

    function parChanged()
        % A value changed: the fields, the plot and the motor performance follow at once
        refreshParFields();
        updateRangePlot();
        if ~isempty(perfFig) && isvalid(perfFig)
            runPerf();
        end
    end

    function parReset()
        parEdit = struct('set', [false false], 'A', [NaN NaN], 'B', [NaN NaN]);
        parChanged();
    end

    function parMonitor()
        parEdit.set = [true, true];
        parEdit.A = [livePref('ptScale', 494.586), livePref('lcScaleN', 1196.14 * lbfToN)];
        parEdit.B = [0, 0];
        parChanged();
    end

    function writeCalToFile()
        % Puts the values of the panel into the calibration blocks of the loaded .BIN file. The raw samples do not change.
        if isempty(plotPath) || ~endsWith(plotPath, '.BIN', 'IgnoreCase', true) || ~isfile(plotPath)
            uialert(filesFig, 'Select a downloaded .BIN file in the list first.', 'Nothing Selected');
            return;
        end
        if ~any(parEdit.set)
            uialert(filesFig, 'No value was edited. Change a value in the fields first (the file already has the values that it shows).', ...
                'Nothing to Write');
            return;
        end
        isSet = parEdit.set | ([plotCal.ptSet, plotCal.lcSet] & plotCal.valid);   % a sensor without a calibration stays without one
        vals = [1, 0, 1, 0];                                % PT A, PT B, LC A, LC B (A = 1, B = 0: the default of the DAQ)
        sensName = {'PT', 'LC'}; unitTxt = {'A in PSI per V, B in PSI', 'A in N per mV/V, B in N'};
        lines = cell(1, 2);
        for q = 1:2
            if isSet(q)
                [a, b] = sensorParams(q);
                vals(2*q-1:2*q) = [a, b];
                lines{q} = sprintf('%s:  A = %.7g,  B = %.7g   (%s)', sensName{q}, a, b, unitTxt{q});
            else
                lines{q} = sprintf('%s:  no calibration', sensName{q});
            end
        end
        if plotCal.nBlocks > 0
            what = sprintf('This REPLACES the calibration inside the file, in all %d places where it is stored.', plotCal.nBlocks);
        else
            what = 'The file has no calibration. One calibration block is ADDED at the end of the file.';
        end
        msg = sprintf(['%s\n\nNew values:\n%s\n%s\n\nThe raw samples do not change. A copy of the original file is kept as\n%s.orig ', ...
            '(made once).\nThe calibration saved on the DAQ does NOT change.\nEvery program that reads this file will use the new ', ...
            'values.\n\nOverwrite the calibration in %s?'], what, lines{1}, lines{2}, plotName, plotName);
        choice = uiconfirm(filesFig, msg, 'Overwrite the calibration in the file', 'Options', {'Overwrite', 'Cancel'}, ...
            'DefaultOption', 2, 'CancelOption', 2, 'Icon', 'warning');
        if ~strcmp(choice, 'Overwrite')
            return;
        end
        try
            bak = [plotPath, '.orig'];
            if ~isfile(bak)
                [okCopy, copyMsg] = copyfile(plotPath, bak);
                if ~okCopy
                    error('The backup copy failed: %s', copyMsg);
                end
            end
            nBlk = patchCalBlocks(plotPath, isSet, vals);
        catch ME
            uialert(filesFig, ME.message, 'Write Problem');
            return;
        end
        range0 = [startSlider.Value, endSlider.Value];
        loadSelected();                                     % reads the file again: its calibration is now the edited one
        lim = startSlider.Limits;
        startSlider.Value = min(max(range0(1), lim(1)), lim(2));
        endSlider.Value = min(max(range0(2), lim(1)), lim(2));
        updateRangePlot();
        got = [plotCal.ptA, plotCal.ptB, plotCal.lcA, plotCal.lcB];
        if plotCal.valid && isequal(single(got), single(vals))
            uialert(filesFig, sprintf('Done: %d calibration block(s) written.\nOriginal file: %s', nBlk, bak), 'Calibration Written', 'Icon', 'success');
        else
            uialert(filesFig, 'The file was written, but reading it again gave other values. Check the file (the original is in the .orig file).', ...
                'Check Failed', 'Icon', 'warning');
        end
        if ~isempty(perfFig) && isvalid(perfFig)
            runPerf();
        end
    end

    function n = patchCalBlocks(fpath, isSet, vals)
        % Rewrites the payload of the 6 META records of every valid calibration block (the places stay), or adds one block at the end
        % when the file has none (after the last whole record: a part of a record at the end of the file, which is usual in a downloaded
        % file, stays at the end). The same layout as calFromMeta reads and cal_make_block (src/cal.c) writes: kind 1 = flags (bit 0 PT,
        % bit 1 load cell) << 24 | revision, kinds 2 to 5 = float32 PT A, PT B, LC A, LC B, kind 6 = CRC-32 of the 20 bytes before it.
        fid = fopen(fpath, 'r');
        if fid == -1
            error('Cannot open %s.', fpath);
        end
        bytes = fread(fid, inf, 'uint8=>uint8');
        fclose(fid);
        if numel(bytes) < 8
            error('The file has no records.');
        end
        nr = floor(numel(bytes) / 8);                       % a downloaded file can end with a part of a record: it is not touched
        rec = reshape(bytes(1:nr * 8), 8, nr);
        ts = double(typecast(reshape(rec(1:4, :), [], 1), 'uint32'));
        isMeta = ts >= 4294967280;                          % 0xFFFFFFF0
        blkIdx = [];
        rev = 0;
        if any(isMeta)
            [c0, blkIdx] = calFromMeta(ts(isMeta) - 4294967280, rec(5:8, isMeta), find(isMeta));
            if c0.valid, rev = c0.rev; end
        end
        rev = mod(rev + 1, 2^24);                           % like a Save to DAQ: the revision goes up by one
        hd = bitor(bitshift(uint32(isSet(1) + 2 * isSet(2)), 24), uint32(rev));
        pay = zeros(6, 4, 'uint8');                         % row = the 4 payload bytes of one META record
        pay(1, :) = typecast(hd, 'uint8');
        for i = 1:4
            pay(i + 1, :) = typecast(single(vals(i)), 'uint8');
        end
        crc = crc32Of(reshape(pay(1:5, :)', 1, []));        % the 20 bytes in the order of the file
        pay(6, :) = typecast(uint32(crc), 'uint8');
        fid = fopen(fpath, 'r+');
        if fid == -1
            error('Cannot write %s (is it read-only?).', fpath);
        end
        try
            if isempty(blkIdx)
                fseek(fid, nr * 8, 'bof');                  % after the last whole record
                for j = 1:6
                    fwrite(fid, [typecast(uint32(4294967280 + j), 'uint8'), pay(j, :)], 'uint8');
                end
                fwrite(fid, bytes(nr * 8 + 1:end), 'uint8'); % the part of a record at the end (if any) moves behind the new block
                n = 1;
            else
                for i = 1:numel(blkIdx)
                    for j = 1:6
                        fseek(fid, (blkIdx(i) + j - 2) * 8 + 4, 'bof');   % the payload is bytes 5 to 8 of record number blkIdx + j - 1
                        fwrite(fid, pay(j, :), 'uint8');
                    end
                end
                n = numel(blkIdx);
            end
        catch ME
            fclose(fid);
            rethrow(ME);
        end
        fclose(fid);
    end

    % =====================================================================
    % --- Motor performance (the file of Files & Plot) ---
    % =====================================================================
    % The same analysis as the Motor performance window of DAQ_Live_Monitor (burnStats at the end of this file), on all
    % the records of the loaded file inside the Start/End range (the whole file when the sliders are at their ends).
    % Thrust (N) = LC A * mV/V + B. Pressure (psi) = PT A * V + B. A and B are the sensor parameters of the Files & Plot panel
    % (edited, file cal or app cal): the same values as the plot and the CSV export. The baseline before the burn is removed,
    % so B has no effect, but A does.
    function openPerf(~, ~)
        if isempty(perfFig) || ~isvalid(perfFig)
            perfFig = uifigure('Name', 'Motor performance (file)', 'Position', [170, 70, 1050, 720]);
            uibutton(perfFig, 'Position', [20, 688, 110, 24], 'Text', 'Analyze again', 'ButtonPushedFcn', @(~, ~) runPerf());
            uibutton(perfFig, 'Position', [140, 688, 110, 24], 'Text', 'Save results', 'ButtonPushedFcn', @(~, ~) savePerf());
            perfFrqBtn = uibutton(perfFig, 'state', 'Position', [260, 688, 95, 24], 'Text', 'Frequency', ...
                'Tooltip', 'Spectrum of a 0.5 s window of the data; a slider moves the window', 'ValueChangedFcn', @(~, ~) perfFrqToggle());
            perfLbl = uilabel(perfFig, 'Position', [365, 688, 665, 24], 'Text', '');
            perfAxT = uiaxes(perfFig, 'Position', [20, 360, 640, 320]);
            perfAxP = uiaxes(perfFig, 'Position', [20, 20, 640, 330]);
            perfTxt = uitextarea(perfFig, 'Position', [675, 20, 355, 660], 'Editable', 'off', 'FontName', 'Courier New');
            % the Frequency tool (hidden until the button is on)
            perfAxFT = uiaxes(perfFig, 'Position', [20, 110, 315, 240], 'Visible', 'off');
            perfAxFP = uiaxes(perfFig, 'Position', [345, 110, 315, 240], 'Visible', 'off');
            perfFrqLbl = uilabel(perfFig, 'Position', [20, 82, 640, 22], 'Text', '', 'FontWeight', 'bold', 'Visible', 'off');
            perfFrqSld = uislider(perfFig, 'Position', [30, 72, 620, 3], 'Limits', [0, 1], 'Value', 0, 'Visible', 'off', ...
                'ValueChangingFcn', @(~, e) perfFrqMove(e.Value), 'ValueChangedFcn', @(src, ~) perfFrqMove(src.Value));
            perfFrqTxt = uilabel(perfFig, 'Position', [20, 4, 640, 42], 'Text', '', 'WordWrap', 'on', 'VerticalAlignment', 'top', 'Visible', 'off');
        end
        figure(perfFig);
        runPerf();
    end

    function v = livePref(name, def)
        % A scale of DAQ_Live_Monitor (its MATLAB preferences on this PC), so the two windows give the same numbers.
        % The names ptScale and lcScaleN: the live monitor saved lbf values under lcScale before, they are not used.
        v = def;
        try
            if ispref('DAQ_Live_Monitor', name)
                p = getpref('DAQ_Live_Monitor', name);
                if isnumeric(p) && isscalar(p) && isfinite(p) && p ~= 0
                    v = double(p);
                end
            end
        catch
        end
    end

    function [t0, t1, txt] = perfRange()
        % The Start/End range of Files & Plot. Sliders at their ends = the whole file (the PT and the load cell can end
        % at different times, and the slider limits come from one sensor only).
        t0 = -Inf; t1 = Inf; txt = 'Range: the whole file';
        if isempty(startSlider) || ~isvalid(startSlider) || isempty(plotT)
            return;
        end
        a = startSlider.Value; b = endSlider.Value;
        if a > startSlider.Limits(1), t0 = a; end
        if b < endSlider.Limits(2), t1 = b; end
        if isfinite(t0) || isfinite(t1)
            txt = sprintf('Range: %.3f to %.3f s (Start/End sliders)', a, b);
        end
    end

    function r = perfOne(k, t0, t1)
        % burnStats of sensor k (1 = PT in psi, 2 = load cell in N) of the loaded file, inside t0..t1
        t = plotSets(k).t(:); raw = plotSets(k).raw(:); sq = plotSets(k).seq(:);
        sel = t >= t0 & t <= t1 & isfinite(raw);
        t = t(sel); raw = raw(sel); sq = sq(sel);
        [t, i] = sort(t); raw = raw(i); sq = sq(i);
        [pa, pb, ps] = sensorParams(k);
        if k == 1
            y = pa * raw * 0.1875e-3 + pb;
        else
            y = pa * raw * lcMvVPerCount + pb;
        end
        minAmps = [5, 5];                          % psi, N: a smaller peak is not a motor burn
        r = burnStats(t, y, minAmps(k));
        r.n = numel(t);
        r.pA = pa; r.pB = pb; r.pSrc = ps;           % the sensor parameters of this analysis
        r.rate = 0; r.gapLim = 0.05; r.lost = 0; r.fs = NaN;
        if r.n >= 2 && t(end) > t(1)
            r.rate = (r.n - 1) / (t(end) - t(1));
            dts = diff(t);
            r.fs = 1 / median(dts(dts > 0));                 % the sample rate for the spectrum
            r.gapLim = max(0.05, 10 * median(diff(t)));   % a gap: 10 sample intervals, 50 ms at least
        end
        if r.n == 0
            r.msg = 'no records of this sensor in the file (or in the range)';
        end
        if r.found
            r.tAll = t; r.yAll = y - r.base;
            kb = t >= r.tStart & t <= r.tEnd;
            ds = mod(diff(sq(kb)), 32768 - (k == 2) * (32768 - 128));   % PT seq: 15 bits, load cell seq: 7 bits
            r.lost = sum(ds(ds > 1) - 1);
        end
    end

    function runPerf()
        if isempty(perfFig) || ~isvalid(perfFig)
            return;
        end
        perfRes = [];
        perfLbl.Text = '';
        if isempty(plotSets(1).t) && isempty(plotSets(2).t)
            perfTxt.Value = {'No file is loaded.', 'Select a .BIN file in Files & Plot (Local Files), then push Analyze again.'};
            cla(perfAxT); cla(perfAxP); legend(perfAxT, 'off'); legend(perfAxP, 'off');
            perfFrqUpdate();
            return;
        end
        [t0, t1, rangeTxt] = perfRange();
        rF = perfOne(2, t0, t1);
        rP = perfOne(1, t0, t1);
        if rF.found, tRef = rF.tStart; elseif rP.found, tRef = rP.tStart; else, tRef = 0; end
        L = {sprintf('Motor performance  %s', char(datetime('now', 'Format', 'yyyy-MM-dd HH:mm:ss'))), ...
            ['File: ', plotName], rangeTxt, ...
            sprintf('PT: A = %.7g psi/V, B = %.7g psi  [%s]', rP.pA, rP.pB, rP.pSrc), ...
            sprintf('LC: A = %.7g N per mV/V, B = %.7g N  [%s]', rF.pA, rF.pB, rF.pSrc), ''};
        f = '%-24s %s';
        if rF.found
            I = rF.integral;
            [cls, pct] = motorClass(I);
            L = [L, {sprintf('THRUST  (load cell, %.0f samples/s)', rF.rate), ...
                sprintf(f, 'Burn start', sprintf('%.3f s', rF.tStart)), ...
                sprintf(f, 'Burn end', sprintf('%.3f s', rF.tEnd)), ...
                sprintf(f, 'Burn time (5%-5%)', sprintf('%.3f s', rF.burnS)), ...
                sprintf(f, 'Peak (top 5% avg)', sprintf('%.1f N  (%.1f lbf)', rF.top5, rF.top5 / lbfToN)), ...
                sprintf(f, 'Max sample', sprintf('%.1f N  (%.1f lbf) at %.3f s', rF.maxVal, rF.maxVal / lbfToN, rF.tMax)), ...
                sprintf(f, 'Average thrust', sprintf('%.1f N  (%.1f lbf)', rF.avg, rF.avg / lbfToN)), ...
                sprintf(f, 'Initial thrust (0.5 s)', sprintf('%.1f N  (%.1f lbf)', rF.initial, rF.initial / lbfToN)), ...
                sprintf(f, 'Total impulse', sprintf('%.1f N s  (%.1f lbf s)', I, I / lbfToN)), ...
                sprintf(f, 'Motor class', sprintf('%s  (%.0f %% of %s)', cls, pct, cls)), ...
                sprintf(f, 'Motor name', sprintf('%s%.0f', cls, rF.avg)), ...
                sprintf(f, 'Baseline removed', sprintf('%.2f N  (%.2f lbf)', rF.base, rF.base / lbfToN)), ...
                sprintf(f, 'Peak / noise', sprintf('%.0f x', rF.strength)), ...
                sprintf(f, 'Lost samples in burn', sprintf('%d', rF.lost))}];
            L{end+1} = '';
        else
            L = [L, {'THRUST: no strong peak found', rF.msg, ''}];
        end
        if rP.found
            L = [L, {sprintf('PRESSURE  (PT, %.0f samples/s)', rP.rate), ...
                sprintf(f, 'Peak (top 5% avg)', sprintf('%.1f psi', rP.top5)), ...
                sprintf(f, 'Max sample', sprintf('%.1f psi at %.3f s', rP.maxVal, rP.tMax)), ...
                sprintf(f, 'Average (5%-5%)', sprintf('%.1f psi', rP.avg)), ...
                sprintf(f, 'Pressure burn time', sprintf('%.3f s (%.3f to %.3f s)', rP.burnS, rP.tStart, rP.tEnd)), ...
                sprintf(f, 'Baseline removed', sprintf('%.2f psi', rP.base)), ...
                sprintf(f, 'Peak / noise', sprintf('%.0f x', rP.strength)), ...
                sprintf(f, 'Lost samples in burn', sprintf('%d', rP.lost)), ''}];
        else
            L = [L, {'PRESSURE: no strong peak found', rP.msg, ''}];
        end
        L{end+1} = 'CHECKS';
        if rF.found && rP.found
            dt = rP.tPeak - rF.tPeak;
            if rP.tPeak >= rF.tStart - 1 && rP.tPeak <= rF.tEnd + 1
                L{end+1} = sprintf('OK: both peaks found. The pressure peak is %.2f s from the thrust peak.', dt);
            else
                L{end+1} = sprintf('WARNING: the pressure peak is %.2f s from the thrust peak, outside the burn.', dt);
            end
        else
            L{end+1} = 'WARNING: a strong peak was not found in both pressure and thrust.';
        end
        for r = {rF, rP}
            q = r{1};
            if ~q.found, continue; end
            if q.cutStart, L{end+1} = 'WARNING: the data starts inside the burn (move the Start slider back).'; end   %#ok<AGROW>
            if q.cutEnd, L{end+1} = 'WARNING: the data ends inside the burn (move the End slider forward).'; end      %#ok<AGROW>
            if q.gapMax > q.gapLim
                L{end+1} = sprintf('WARNING: a data gap of %.3f s is inside the burn (straight line over it).', q.gapMax); %#ok<AGROW>
            end
            if q.lost > 0
                L{end+1} = sprintf('WARNING: %d samples are lost inside the burn (seq gaps).', q.lost); %#ok<AGROW>
            end
        end
        pr = {rP, rF}; pu = {'psi', 'N'}; pn = {'PT', 'LC'};
        for i2 = 1:2
            if pr{i2}.n > 0 && strcmp(pr{i2}.pSrc, 'app cal') && pr{i2}.pA == 1 && pr{i2}.pB == 0
                L{end+1} = sprintf(['WARNING: the %s has no calibration in the file and none was typed (A = 1, B = 0): the values are not in %s. ', ...
                    'Edit the sensor parameters in Files & Plot.'], pn{i2}, pu{i2});     %#ok<AGROW>
            end
        end
        if endsWith(plotName, '.csv', 'IgnoreCase', true)
            L = [L, {'', 'This is a live CSV of the app: decimated, one sensor.', 'Use the NAND or SD .BIN file for the final numbers.'}];
        end
        perfTxt.Value = L(:);
        plotFileBurn(perfAxT, rF, tRef, 2);
        plotFileBurn(perfAxP, rP, tRef, 1);
        perfRes = struct('rF', rF, 'rP', rP, 'tRef', tRef);
        perfFrqSetup();
    end

    % --- Frequency tool: the spectrum of a 0.5 s window, centred at the slider ---
    % The button "Frequency" shows a slider (time from the burn start, as on the plots) and the spectrum of the thrust and of the
    % pressure in the window of 0.5 s around it. The frequencies go from 0 to the Nyquist frequency of each sensor (half its sample
    % rate: load cell about 160 Hz, PT about 441 Hz). The spectrum is taken from the raw samples (windowSpectrum): nothing is
    % interpolated. A grey band on the two plots shows the window. The window is 0.5 s, so two tones closer than about 2 Hz
    % are one peak. A vibration above the Nyquist frequency folds back to a lower frequency: it cannot be told from a real one.
    function perfFrqToggle()
        on = perfFrqBtn.Value;
        if on
            perfAxT.Position = [20, 520, 640, 160];
            perfAxP.Position = [20, 355, 640, 160];
            vis = 'on';
        else
            perfAxT.Position = [20, 360, 640, 320];
            perfAxP.Position = [20, 20, 640, 330];
            vis = 'off';
            for q = 1:2
                if isgraphics(perfFrqMk(q)), delete(perfFrqMk(q)); end
            end
        end
        perfAxFT.Visible = vis; perfAxFP.Visible = vis;
        perfFrqSld.Visible = vis; perfFrqLbl.Visible = vis; perfFrqTxt.Visible = vis;
        if on
            perfFrqUpdate();
        end
    end

    function perfFrqSetup()
        % After each analysis: the slider covers the plotted time (2 s before the burn start to 2 s after the burn end). The window
        % stays where it was when it is still inside, else it goes to the middle of the burn.
        if isempty(perfFrqSld) || ~isvalid(perfFrqSld)
            return;
        end
        lo = Inf; hi = -Inf;
        rs = {perfRes.rF, perfRes.rP};
        for q = 1:2
            if rs{q}.found
                lo = min(lo, rs{q}.tStart); hi = max(hi, rs{q}.tEnd);
            end
        end
        if ~isfinite(lo)
            perfFrqSld.Enable = 'off';
            perfFrqPos = NaN;
        else
            x0 = lo - 2 - perfRes.tRef; x1 = hi + 2 - perfRes.tRef;
            perfFrqSld.Limits = [x0, x1];
            if isnan(perfFrqPos) || perfFrqPos < x0 || perfFrqPos > x1
                perfFrqPos = (lo + hi) / 2 - perfRes.tRef;
            end
            perfFrqSld.Value = perfFrqPos;
            perfFrqSld.Enable = 'on';
        end
        perfFrqUpdate();
    end

    function perfFrqMove(v)
        perfFrqPos = v;
        perfFrqUpdate();
    end

    function perfFrqUpdate()
        % Draws the two spectra for the window at perfFrqPos. A busy flag skips the slider events that come while it draws.
        if isempty(perfFrqBtn) || ~isvalid(perfFrqBtn) || ~perfFrqBtn.Value || perfFrqBusy
            return;
        end
        perfFrqBusy = true;
        try
            axs = {perfAxFT, perfAxFP};
            tAx = {perfAxT, perfAxP};
            if isempty(perfRes) || isnan(perfFrqPos)
                cla(axs{1}); cla(axs{2});
                perfFrqLbl.Text = 'No burn found: nothing to analyze.';
                perfFrqTxt.Text = '';
                for q = 1:2
                    if isgraphics(perfFrqMk(q)), delete(perfFrqMk(q)); end
                end
                perfFrqBusy = false;
                return;
            end
            winS = 0.5;
            tc = perfRes.tRef + perfFrqPos;
            rs = {perfRes.rF, perfRes.rP};
            nm = {'Thrust', 'Pressure'}; un = {'N', 'psi'};
            col = {[0 0.447 0.741], [0.850 0.325 0.098]};
            lines = cell(1, 2);
            for q = 1:2
                ax = axs{q}; rq = rs{q};
                cla(ax);
                xlabel(ax, 'Frequency (Hz)');
                ylabel(ax, sprintf('Amplitude (%s)', un{q}));
                grid(ax, 'on');
                if isgraphics(perfFrqMk(q)), delete(perfFrqMk(q)); end
                if ~rq.found
                    title(ax, sprintf('%s: no strong peak found', nm{q}));
                    lines{q} = sprintf('%s: no strong peak found', nm{q});
                    continue;
                end
                % the window on the time plot
                yl = ylim(tAx{q});
                xa = perfFrqPos - winS / 2; xb = perfFrqPos + winS / 2;
                perfFrqMk(q) = patch(tAx{q}, [xa xb xb xa], yl([1 1 2 2]), [0.2 0.2 0.2], 'FaceAlpha', 0.25, 'EdgeColor', 'none', 'HitTest', 'off');
                perfFrqMk(q).Annotation.LegendInformation.IconDisplayStyle = 'off';
                [f, amp, nyq, msg] = windowSpectrum(rq.tAll, rq.yAll, rq.fs, tc, winS);
                if isempty(f)
                    title(ax, sprintf('%s: %s', nm{q}, msg));
                    lines{q} = sprintf('%s: %s', nm{q}, msg);
                    continue;
                end
                plot(ax, f, amp, '-', 'Color', col{q}, 'LineWidth', 1.1);
                set(ax, 'YScale', 'log');
                top = max(amp);
                if ~(top > 0), top = 1; end
                ylim(ax, [top * 1e-4, top * 2]);
                xlim(ax, [0, nyq]);
                title(ax, sprintf('%s spectrum (Nyquist %.0f Hz)', nm{q}, nyq));
                lines{q} = sprintf('%s peaks: %s', nm{q}, topPeaks(f, amp, 6, un{q}));
            end
            perfFrqLbl.Text = sprintf('0.5 s window at %+.3f s from the burn start (file time %.3f s). Resolution about 2 Hz.', perfFrqPos, tc);
            perfFrqTxt.Text = {lines{1}, lines{2}};
        catch ME
            perfFrqTxt.Text = ['Frequency analysis failed: ', ME.message];
        end
        perfFrqBusy = false;
    end

    function plotFileBurn(ax, r, tRef, k)
        cla(ax); legend(ax, 'off'); hold(ax, 'on'); grid(ax, 'on');
        if k == 2, nm = 'Thrust'; u = 'N'; src = 'load cell'; col = [0 0.447 0.741];
        else, nm = 'Pressure'; u = 'psi'; src = 'PT'; col = [0.850 0.325 0.098]; end
        xlabel(ax, 'Time from the burn start (s)');
        ylabel(ax, sprintf('%s above the baseline (%s)', nm, u));
        if ~r.found
            title(ax, sprintf('%s: no strong peak found', nm));
            return;
        end
        sel = r.tAll >= r.tStart - 2 & r.tAll <= r.tEnd + 2;
        x = r.tAll(sel) - tRef; y = r.yAll(sel);
        yl = [min(0, min(y)), max(y) * 1.1];
        patch(ax, [r.tStart r.tEnd r.tEnd r.tStart] - tRef, yl([1 1 2 2]), [0.85 0.92 1], 'EdgeColor', 'none');
        plot(ax, x, y, '-', 'Color', col, 'LineWidth', 1.1);
        plot(ax, [x(1) x(end)], r.top5 * [1 1], '--', 'Color', [0.8 0 0], 'LineWidth', 1);
        plot(ax, [r.tStart r.tEnd] - tRef, r.avg * [1 1], '--', 'Color', [0 0.5 0], 'LineWidth', 1);
        legend(ax, {'burn (5 % to 5 %)', src, 'top 5 % avg', 'average'}, 'Location', 'northeast');
        ylim(ax, yl);
        xlim(ax, [x(1), max(x(end), x(1) + 1e-3)]);
        if k == 2
            title(ax, sprintf('Thrust: peak %.0f N (%.0f lbf), avg %.0f N, impulse %.0f N s (%.0f lbf s), burn %.2f s', r.top5, r.top5 / lbfToN, r.avg, r.integral, r.integral / lbfToN, r.burnS));
        else
            title(ax, sprintf('Pressure: peak %.0f psi, avg %.0f psi', r.top5, r.avg));
        end
    end

    function savePerf()
        if isempty(perfRes)
            perfLbl.Text = 'Nothing to save: load a file, then push Analyze again.';
            return;
        end
        [~, fb] = fileparts(plotName);
        base = fullfile(outDir, ['motor_performance_', fb, '_', char(datetime('now', 'Format', 'yyyy-MM-dd_HH-mm-ss'))]);
        fid = fopen([base, '.txt'], 'w');
        if fid == -1
            perfLbl.Text = ['Cannot write ', base, '.txt'];
            return;
        end
        fprintf(fid, '%s\n', perfTxt.Value{:});
        fclose(fid);
        fid = fopen([base, '_curve.csv'], 'w');
        if fid ~= -1
            fprintf(fid, 'series,time_s,time_from_burn_start_s,value,unit\n');
            rs = {perfRes.rF, perfRes.rP}; nm = {'thrust', 'pressure'}; un = {'N', 'psi'};
            for i = 1:2
                r = rs{i};
                if ~r.found, continue; end
                sel = r.tAll >= r.tStart - 2 & r.tAll <= r.tEnd + 2;
                fprintf(fid, [nm{i}, ',%.6f,%.6f,%.6g,', un{i}, '\n'], [r.tAll(sel)'; r.tAll(sel)' - perfRes.tRef; r.yAll(sel)']);
            end
            fclose(fid);
        end
        try
            exportgraphics(perfAxT, [base, '_thrust.png']);
            exportgraphics(perfAxP, [base, '_pressure.png']);
        catch
        end
        msg = '';
        try
            exportBurnPdf([base, '.pdf'], ['Static fire: ', plotName], perfRes.rF, perfRes.rP, perfRes.tRef, ...
                burnSummary(plotName, perfRes.rF, perfRes.rP));
        catch ME
            msg = ['  (PDF failed: ', ME.message, ')'];
        end
        perfLbl.Text = ['Saved: ', base, '.txt, _curve.csv, .png, .pdf', msg];
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
        if ~isempty(perfFig) && isvalid(perfFig)
            delete(perfFig);
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

% ========================================================================= burn analysis
% A copy of burnStats, crossTime and motorClass in src/DAQ_Live_Monitor.m (each app is one file). Change both together.
% The strongest peak of y(t) and its burn. minAmp: the smallest peak (above the baseline) that counts.
%   Noise    sigma from the differences of neighbour samples (the burn does not change it much).
%   Peak     the maximum of a 5-point moving median (one wrong sample is not a peak).
%   Baseline the median of the 2 s that end 0.2 s before the burn start. It is removed from all values.
%   Burn     from where the curve passes 5 % of the peak going up, to where it goes below 5 % after the peak
%            (crossing times are interpolated).
%   top5     the mean of the highest 5 % of the burn samples. avg = integral / burn time. integral: trapezoids.
%   initial  the mean of the first 0.5 s of the burn.
function r = burnStats(t, y, minAmp)
    r = struct('found', false, 'msg', '', 'strength', 0, 'base', 0, 'tStart', NaN, 'tEnd', NaN, 'burnS', NaN, ...
        'tPeak', NaN, 'tMax', NaN, 'maxVal', NaN, 'top5', NaN, 'avg', NaN, 'integral', NaN, 'gapMax', 0, ...
        'cutStart', false, 'cutEnd', false, 'initial', NaN, 'tAll', [], 'yAll', []);
    n = numel(t);
    if n < 20, r.msg = 'not enough data'; return; end
    t = reshape(t, 1, []); y = reshape(y, 1, []);
    ys = movmedian(y, 5);
    dy = diff(y);
    sigma = 1.4826 * median(abs(dy)) / sqrt(2);
    if sigma == 0, sigma = std(dy) / sqrt(2); end
    [pk, ip] = max(ys);
    sigma = max(sigma, 1e-9 * max(1, abs(pk)));
    base0 = median(y);
    if pk <= base0, r.msg = 'no peak'; return; end
    thr = base0 + 0.05 * (pk - base0);
    i0 = find(ys(1:ip) < thr, 1, 'last');
    if isempty(i0), i0 = 1; else, i0 = i0 + 1; end
    kb = t >= t(i0) - 2.2 & t < t(i0) - 0.2;
    if nnz(kb) >= 5, base = median(y(kb)); else, base = base0; end
    amp = pk - base;
    r.strength = amp / sigma;
    r.base = base;
    if amp < minAmp || r.strength < 15
        r.msg = sprintf('no strong peak (%.3g above the baseline, %.0f x noise)', amp, r.strength);
        return;
    end
    thr = base + 0.05 * amp;
    i0 = find(ys(1:ip) < thr, 1, 'last');
    r.cutStart = isempty(i0);
    if r.cutStart, i0 = 1; else, i0 = i0 + 1; end
    j = find(ys(ip:end) < thr, 1, 'first');
    r.cutEnd = isempty(j);
    if r.cutEnd, i1 = n; else, i1 = ip + j - 2; end
    if i1 - i0 < 2, r.msg = 'the peak is too short for a burn'; return; end
    yn = y - base;
    thrN = thr - base;
    tt = t(i0:i1); yy = yn(i0:i1);
    if ~r.cutStart
        tS = crossTime(t(i0-1), ys(i0-1), t(i0), ys(i0), thr);
        tt = [tS, tt]; yy = [thrN, yy];
    end
    if ~r.cutEnd
        tE = crossTime(t(i1), ys(i1), t(i1+1), ys(i1+1), thr);
        tt = [tt, tE]; yy = [yy, thrN];
    end
    r.tStart = tt(1); r.tEnd = tt(end);
    r.burnS = r.tEnd - r.tStart;
    srt = sort(yn(i0:i1), 'descend');
    r.top5 = mean(srt(1:max(1, ceil(0.05 * numel(srt)))));
    r.integral = trapz(tt, yy);
    r.avg = r.integral / max(r.burnS, eps);
    [r.maxVal, im] = max(yn(i0:i1));
    tb = t(i0:i1); yb = yn(i0:i1);
    kI = tb <= r.tStart + 0.5;                 % initial thrust: the mean of the FIRST 0.5 s of the burn (t <= start + 0.5 s)
    if ~any(kI), kI(1) = true; end
    r.initial = mean(yb(kI));
    r.tMax = t(i0 + im - 1);
    r.tPeak = t(ip);
    r.gapMax = max(diff(tt));
    r.found = true;
end

function tc = crossTime(ta, ya, tb, yb, thr)
    if yb == ya, tc = tb; return; end
    tc = ta + (thr - ya) / (yb - ya) * (tb - ta);
    tc = min(max(tc, min(ta, tb)), max(ta, tb));
end

% Motor class from the total impulse in N s: A = 1.26..2.5, B = 2.51..5, each next letter doubles.
function [c, pct] = motorClass(I)
    if I <= 2.5
        c = 'A'; pct = 100 * I / 2.5;
        return;
    end
    k = ceil(log2(I / 2.5));
    c = char('A' + k);
    pct = 100 * I / (2.5 * 2^k);
end

% The short report of the old StaticFirePerf script (same words): file, motor name (class letter + average thrust in N),
% total impulse, max and average thrust, initial thrust (first 0.5 s), burn time, max and average pressure.
% rF: burnStats of the thrust (N), rP: burnStats of the pressure (psi). lbf (second unit) is in brackets.
function L = burnSummary(name, rF, rP)
    lb = 4.4482216152605;
    L = {['File: ', name]};
    if rF.found
        cls = motorClass(rF.integral);
        L = [L, {sprintf('Motor Name: %s%.0f', cls, rF.avg), ...
            sprintf('Total Impulse: %.2f N%cs  (%.2f lbf%cs)', rF.integral, 183, rF.integral / lb, 183), ...
            sprintf('Max Thrust: %.2f N  (%.2f lbf)', rF.maxVal, rF.maxVal / lb), ...
            sprintf('Average Thrust: %.2f N  (%.2f lbf)', rF.avg, rF.avg / lb), ...
            sprintf('Initial Thrust (0.5 s): %.2f N  (%.2f lbf)', rF.initial, rF.initial / lb), ...
            sprintf('Burn Time: %.2f s', rF.burnS)}];
    else
        L{end+1} = 'Thrust: no strong peak found';
    end
    if rP.found
        L = [L, {sprintf('Max Pressure: %.2f psi', rP.maxVal), sprintf('Average Pressure: %.2f psi', rP.avg)}];
    else
        L{end+1} = 'Pressure: no strong peak found';
    end
end

% The PDF page of the old StaticFirePerf script: thrust (left axis, N) and pressure (right axis, psi) against the time from the
% burn start, and the short report under the plot. tRef: the time of the burn start. summary: the lines of burnSummary.
function exportBurnPdf(pdfFile, titleStr, rF, rP, tRef, summary)
    fig = figure('Visible', 'off', 'Position', [100 100 900 650]);
    guard = onCleanup(@() close(fig));            %#ok<NASGU> closes the figure also when an error happens below
    ax = axes(fig);
    cT = [0.85 0 0]; cP = [0 0.45 0.74];
    yyaxis(ax, 'left');
    if rF.found
        k = rF.tAll >= rF.tStart - 2 & rF.tAll <= rF.tEnd + 2;
        plot(ax, rF.tAll(k) - tRef, rF.yAll(k), '-', 'Color', cT);
    end
    ylabel(ax, 'Thrust (N)');
    ax.YAxis(1).Color = cT;
    yyaxis(ax, 'right');
    if rP.found
        k = rP.tAll >= rP.tStart - 2 & rP.tAll <= rP.tEnd + 2;
        plot(ax, rP.tAll(k) - tRef, rP.yAll(k), '-', 'Color', cP);
    end
    ylabel(ax, 'Pressure (psi)');
    ax.YAxis(2).Color = cP;
    xlabel(ax, 'Time from the burn start (s)');
    grid(ax, 'on');
    title(ax, titleStr, 'Interpreter', 'none');
    ax.Position = [0.13 0.42 0.775 0.5];          % room for the report under the plot
    annotation(fig, 'textbox', [0.13 0.02 0.775 0.32], 'String', summary, 'FontSize', 10, 'FontName', 'FixedWidth', ...
        'EdgeColor', 'none', 'VerticalAlignment', 'top', 'Interpreter', 'none');
    exportgraphics(fig, pdfFile, 'ContentType', 'vector');
end

% ========================================================================= 1 kHz resampling
% y(t) at the times tg (column vectors). makima (modified Akima cubic): it uses the neighbour points on both sides, gives no invented
% peak and no negative thrust at the ignition edge and the burnout (a spline does), and keeps more of a fast oscillation than pchip or
% linear (87 Hz tone of a 321 SPS sensor: makima 91 %, pchip 87 %, linear 77 %, spline 97 %; see tools/resample_mirror.py methods).
% Any interpolation loses some amplitude near the Nyquist frequency: for the spectrum the motor window uses the raw samples.
% NaN outside the data and inside a data gap (more than 10 sample intervals and 50 ms at least): nothing is invented there.
% MATLAB before R2019b has no makima: pchip is used then.
function yi = resampleGaps(t, y, tg)
    yi = nan(size(tg));
    ok = isfinite(t) & isfinite(y);
    t = t(ok); y = y(ok);
    [t, iu] = unique(t);                          % sorted, no equal times (pchip needs strictly increasing times)
    y = y(iu);
    if numel(t) < 4
        return;
    end
    try
        yi = interp1(t, y, tg, 'makima', NaN);
    catch
        yi = interp1(t, y, tg, 'pchip', NaN);
    end
    dt = diff(t);
    gl = max(0.05, 10 * median(dt));
    ip = interp1(t, (1:numel(t))', tg, 'previous', NaN);   % the sample before each grid time
    inside = ~isnan(ip);
    big = [dt > gl; false];                       % the interval after sample i is a gap
    bad = ~inside;
    bad(inside) = big(ip(inside));
    yi(bad) = NaN;
end

% ========================================================================= spectrum of a window (motor window, Frequency)
% The amplitude spectrum of y(t) in the window of winS seconds centred at tc, for the frequencies 0 to fs / 2 (the Nyquist frequency)
% in steps of 0.25 Hz. A DFT taken directly at the real sample times: no interpolation, because any interpolation loses amplitude near
% Nyquist (a 130 Hz tone of a 321 SPS sensor loses 20 to 45 %, see tools/resample_mirror.py methods). The mean and the slope of the window
% are removed (the thrust curve is not a resonance), then a Hann window (at the real times). amp: single sided, in the unit of y.
% The window of 0.5 s resolves about 2 Hz. msg says why there is no spectrum (data gap, end of the data).
function [f, amp, nyq, msg] = windowSpectrum(t, y, fs, tc, winS)
    f = []; amp = []; nyq = NaN; msg = '';
    t = t(:); y = y(:);
    if numel(t) < 20 || ~isfinite(fs) || fs <= 0
        msg = 'not enough data';
        return;
    end
    ta = tc - winS / 2; tb = tc + winS / 2;
    i1 = find(t >= ta, 1);
    i2 = find(t <= tb, 1, 'last');
    if isempty(i1) || isempty(i2) || i2 - i1 < 2
        msg = 'the end of the data is inside this window';
        return;
    end
    tw = t(i1:i2); yw = y(i1:i2);
    if max(diff(tw)) > max(0.05, 10 / fs)
        msg = 'a data gap is inside this window';
        return;
    end
    if tw(1) - ta > 3 / fs || tb - tw(end) > 3 / fs
        msg = 'the end of the data is inside this window';
        return;
    end
    p = polyfit(tw - tc, yw, 1);
    yw = yw - polyval(p, tw - tc);                % mean and slope out
    w = 0.5 - 0.5 * cos(2 * pi * (tw - ta) / winS);
    f = (0:0.25:fs / 2)';
    X = exp(-2j * pi * (f * tw')) * (w .* yw);
    amp = 2 * abs(X) / sum(w);
    nyq = fs / 2;
end

% The three biggest peaks above fMin (Hz) as text: local maxima that are at least 5 % of the biggest and 6 Hz apart. unit: of the amplitude.
function s = topPeaks(f, amp, fMin, unit)
    s = '-';
    k = find(f >= fMin);
    a = amp(k);
    if numel(a) < 3
        return;
    end
    isPk = [false; a(2:end-1) > a(1:end-2) & a(2:end-1) >= a(3:end); false];
    idx = find(isPk);
    [~, o] = sort(a(idx), 'descend');
    idx = idx(o);
    got = [];
    for i = idx'
        if a(i) < 0.05 * max(a), break; end
        if all(abs(f(k(got)) - f(k(i))) >= 6)
            got(end+1) = i;   %#ok<AGROW>
        end
        if numel(got) == 3, break; end
    end
    if isempty(got)
        return;
    end
    parts = arrayfun(@(i) sprintf('%.1f Hz (%.3g %s)', f(k(i)), a(i), unit), got, 'UniformOutput', false);
    s = strjoin(parts, ', ');
end
