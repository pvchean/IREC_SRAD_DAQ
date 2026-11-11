function DAQ_Live_Monitor()
%DAQ_LIVE_MONITOR Live view of the static-fire telemetry: pressure and thrust, the history on the server, motor performance.
%
%   DAQ_Live_Monitor
%
%   SOURCE
%   Server (TCP)   This PC connects to the telemetry server as a DOWNLOAD client (frame format: server/telem_proto.h).
%                  No DAQ is necessary in the bunker, but this PC must have internet. Type the host, the port and the
%                  token of the server, then push Connect. The host and the port stay in the MATLAB preferences of this
%                  PC. The token field starts with the team token (defTok below), or with the environment variable
%                  DAQ_TELEM_TOKEN when it is set. A wrong token stops the connection (red banner). After a break of the link the monitor
%                  connects again (1 s, then up to 10 s) and continues at its cursor, so no data is lost while the server
%                  keeps it. Each sample keeps the time of its DAQ (its microsecond clock). A gap in the data is a gap in the line.
%   DAQ on USB     A DAQ in DOWNLOAD mode (set it in DAQ_Calibration_App: WiFi... button, then reboot the DAQ) gets the data
%                  from the server and prints it on USB ("<volts>", "L:<count>", then "T:DTPT", "T:DTLC", "T:LINK" and
%                  "T:AGE" at the end of each batch). The samples of a batch get their time from the T:AGE line and the
%                  sample interval. This source has no history.
%
%   HISTORY (Server only)
%   The server keeps the frames of many hours in memory. "History" selects how much old data Connect loads before the
%   live view starts: none, the last 2, 10 or 30 minutes of data, or all that the server has. The monitor finds the start
%   from the frame rate, so "last 10 min" is about 10 minutes of data. The banner shows the progress. To load the history
%   again (for example after the static fire), select it and push Connect again.
%   "Save & clear server" makes the server close its archive files (they stay on the server), start new ones and empty
%   its history (for example after a very long idle time). The monitor then clears its plots too. An old server refuses it.
%
%   UNITS
%   Pressure (psi) = PT scale (psi/V) * volts - PT tare.     Default scale 494.586 psi/V.
%   Force (lbf)    = LC scale (lbf per mV/V) * mV/V - LC tare. Default scale 1196.14 lbf per mV/V. Thrust shows in lbf or N.
%   "Tare PT" and "Tare LC" set the tare of each DAQ to the mean of its last 2 s of data. The scales and the tares stay
%   in the MATLAB preferences of this PC. The calibration that is saved in the DAQ is not used here.
%
%   PLOTS
%   Several DAQs: each DAQ gets its own colour and a legend entry ("DAQ 5"). Each board has its own clock, so each DAQ
%   has its own time map; two DAQs line up to about 0.5 s. The screen draws 4 times per second at most, and only when
%   the data changed. A line shows 1500 points at most: in a long window, each bucket of samples gives its minimum and
%   its maximum, so peaks stay visible (the same method as the Files & Plot window of DAQ_Calibration_App).
%   The y-axis spans 5 % of the full scale at least (FS psi, FS lbf), so the noise of a quiet sensor stays small.
%   The thin overview under the plots shows all the data in memory: one colour row for pressure and one for thrust,
%   colour = |value| / full scale. The black box is the view. The View slider moves the view (right end = LIVE: it
%   follows the newest data); a click on the overview also moves it. The Width slider sets the width (5 s to 4 h).
%
%   MOTOR PERFORMANCE
%   The button opens a window that finds the strongest peak of thrust and of pressure in all the data in memory and
%   gives: burn time (5 % of peak to 5 % of peak), peak thrust (mean of the top 5 % of the burn samples), average thrust
%   (impulse / burn time), total impulse (N s) and the motor class. The baseline before the burn is removed first.
%   The telemetry data is decimated (about 60 samples/s). Use the SD or NAND file for the final numbers.
%
%   "Data age" is the age of the newest data on the server when the server answered. The banner turns red when the data
%   age is above the limit, or when no batch comes at all. The CSV log (Log to CSV) has each sample with its time.

    % --- constants ---
    lcMvVPerCount = 1000 / 2^31;      % load cell: counts / (gain 128 * 2^24) as mV/V (the same as DAQ_Calibration_App)
    voltPerLsb = 0.0001875;           % PT: ADS1115, +/-6.144 V range (src/stream.c VOLT_PER_LSB)
    lbfToN = 4.4482216152605;
    ptScaleDef = 494.586;             % psi per volt
    lcScaleDef = 1196.14;             % lbf per mV/V
    maxPts = 1500;                    % the most points of one plot line
    drawPeriod = 0.25;                % s between two redraws (at most)
    tareS = 2;                        % a tare is the mean of the last 2 s
    probeFrames = 300;                % history: the frame rate comes from the last ~300 frames on the server
    banner0 = 'Not connected';
    srcTcp = 'Server (TCP)';
    srcUsb = 'DAQ on USB';
    prefGroup = 'DAQ_Live_Monitor';
    grey = [0.55 0.55 0.55]; red = [0.80 0 0]; green = [0 0.55 0.15]; orange = [0.85 0.45 0]; blue = [0.10 0.35 0.65];
    histItems = {'Live only', 'Last 2 min', 'Last 10 min', 'Last 30 min', 'All on server'};
    histSecs = [0, 120, 600, 1800, Inf];
    defTok = 'OIahPM4HCuo1WGu9';      % the token of the team server (DAQ_TELEM_TOKEN replaces it when it is set)
    ptFsDef = 2000;                   % full scale of the PT (psi): the y-axis span is 5 % of it at least
    lcFsDef = 2500;                   % full scale of the load cell (lbf)
    minSpanFrac = 0.05;
    widthMinS = 5; widthMaxS = 4 * 3600;   % the limits of the Width slider

    % --- protocol (server/telem_proto.h) ---
    TP_HELLO = 1; TP_ACK = 2; TP_DATA_PT = 3; TP_DATA_LC = 4; TP_STATUS = 6; TP_GET = 7; TP_BATCH = 8; TP_NAK = 9;
    TP_RESET = 10;
    TP_ROLE_DOWNLOAD = 2;
    TP_NAK_TOKEN = 1;
    TP_CURSOR_NOW = intmax('uint64');
    TP_AGE_NONE = 4294967295;
    META_BASE = 4294967280;           % 0xFFFFFFF0: the time field of a META record (src/record.h)
    getBytes = 15000;                 % max_bytes of a GET (the server takes 15000 at most)
    getWaitMs = 2000;                 % wait_ms of a GET: the server answers at once when it has data, else after this time

    % --- state: common ---
    tmr = [];
    activeSrc = '';                                % '' (not connected), 'tcp' or 'usb'
    t0 = tic; wallT0 = 0;                          % wallT0: the unix time of t0
    ageMs = NaN; lastBatchT = -Inf; nBatches = 0;
    % Device ID 0..7 is index 1..8 (a DAQ on USB is index 1). Sensor 1 = PT (volts), 2 = load cell (mV/V).
    % ser(d, s): T = time (s since Connect), V = value, n = samples in use (T and V grow by doubling), dt = sample interval.
    % A NaN in V is a gap in the line.
    nDev = 8;
    ser0 = struct('T', zeros(1, 0), 'V', zeros(1, 0), 'n', 0, 'dt', 0);
    ser = repmat(ser0, nDev, 2);
    mp0 = struct('haveMap', false, 'rawRef', 0, 'uRef', 0, 'offS', 0);   % the time map of each board clock
    mp = repmat(mp0, 1, nDev);
    pal = [0 0.447 0.741; 0.850 0.325 0.098; 0.929 0.694 0.125; 0.494 0.184 0.556; ...
           0.466 0.674 0.188; 0.301 0.745 0.933; 0.635 0.078 0.184; 0.25 0.25 0.25];       % one colour for each device ID
    hLine = gobjects(nDev, 2);                     % the plot lines: made when the first sample of a DAQ comes
    legKey = {'', ''}; lastLblL = ''; ylCache = {[], []};
    dataVer = 0; drawnVer = -1; lastDraw = -Inf;
    % the view: viewLive = the right edge follows the newest data; else it stays at viewEnd. widthS = the width of the view.
    viewLive = true; viewEnd = 0; widthS = 120; lastDragT = -Inf; ovLast = -Inf;
    resetSentT = -Inf;                             % the time of the last RESET to the server
    tick = 0;
    logFile = -1;
    ptTare = tareVec('ptTare');
    lcTare = tareVec('lcTare');

    % --- state: USB ---
    s = [];
    carry = '';
    pendPT = []; pendLC = [];                      % samples of the batch that is not finished (no T:AGE line yet)
    dtPT = 0; dtLC = 0;                            % sample intervals of the last batch, in s (0 = not known)
    drops = NaN; rssiUp = NaN; rssiOwn = NaN;
    daqRun = ''; daqLink = NaN;
    portName = '';

    % --- state: TCP ---
    tc = [];
    rx = zeros(1, 0, 'uint8');
    tcpState = 'idle';                             % idle, wait (to connect again), hello, run, refused (wrong token)
    tcpMsg = '';                                   % the reason of the last break
    stateT = 0; retryAt = 0; backoffS = 1;
    getOut = false; getT = 0; moreWaiting = false;
    cursor = TP_CURSOR_NOW;
    srvHost = ''; srvPort = 0; srvTok = zeros(1, 16, 'uint8');
    hadLink = false; srvReconnects = 0;
    upSt = struct('rssi', NaN, 'drops', NaN, 'queue', NaN, 'uptime', NaN, 'reconnects', NaN);
    upDev = NaN;                                   % the device ID of the upload DAQ (the source of STATUS)

    % --- state: history load ---
    % loadState: 'start' (no ACK yet), 'probe' (measure the frame rate), 'loading' (old data), 'live'
    loadState = 'live';
    loadFrom = 0; loadTo = 0; loadBytes = 0; ordN = 0; nReboots = 0;
    hb0 = struct('raw', zeros(1, 0), 'v', zeros(1, 0), 's', zeros(1, 0), 'ord', zeros(1, 0), 'n', 0);
    histBuf = repmat(hb0, 1, nDev);                % raw board times of the old data, until the load is complete

    % --- state: motor performance window ---
    perfFig = []; perfAxT = []; perfAxP = []; perfTxt = []; perfLbl = []; perfRes = [];

    if isdeployed
        homeDir = getenv('USERPROFILE');
        if isempty(homeDir), homeDir = getenv('HOME'); end
        outDir = fullfile(homeDir, 'Documents', 'DAQ Calibration');
    else
        outDir = fullfile(pwd, 'DAQ Calibration');
    end
    if ~exist(outDir, 'dir'), mkdir(outDir); end

    % --- UI ---
    fig = uifigure('Name', 'DAQ Live Monitor (telemetry)', 'Position', [80, 40, 1150, 800]);
    statusLbl = uilabel(fig, 'Position', [20, 765, 380, 22], 'Text', 'Status: Disconnected', 'FontWeight', 'bold');
    uilabel(fig, 'Position', [405, 765, 50, 22], 'Text', 'Source:');
    srcDrop = uidropdown(fig, 'Position', [455, 765, 130, 22], 'Items', {srcTcp, srcUsb}, ...
        'Value', prefItem('source', {srcTcp, srcUsb}, srcTcp), 'ValueChangedFcn', @(~, ~) disconnectAll());
    uibutton(fig, 'Position', [595, 765, 90, 22], 'Text', 'Connect', 'ButtonPushedFcn', @onConnect);
    uibutton(fig, 'Position', [695, 765, 90, 22], 'Text', 'Disconnect', 'ButtonPushedFcn', @(~, ~) disconnectAll());
    uibutton(fig, 'Position', [795, 765, 155, 22], 'Text', 'Save & clear server', 'ButtonPushedFcn', @onServerReset, ...
        'Tooltip', 'The server starts new archive files (the old ones stay) and empties its history');
    uibutton(fig, 'Position', [960, 765, 170, 22], 'Text', 'Motor performance...', 'FontWeight', 'bold', ...
        'ButtonPushedFcn', @openPerf);

    uilabel(fig, 'Position', [20, 735, 50, 22], 'Text', 'Server:');
    hostField = uieditfield(fig, 'text', 'Position', [70, 735, 200, 22], 'Value', prefOr('host', ''), ...
        'Placeholder', 'host name or IP');
    uilabel(fig, 'Position', [280, 735, 35, 22], 'Text', 'Port:');
    portField = uieditfield(fig, 'numeric', 'Position', [315, 735, 65, 22], 'Value', prefOr('port', 48613), ...
        'Limits', [1 65535], 'RoundFractionalValues', 'on', 'ValueDisplayFormat', '%d');
    uilabel(fig, 'Position', [392, 735, 45, 22], 'Text', 'Token:');
    tok0 = getenv('DAQ_TELEM_TOKEN');
    if isempty(tok0), tok0 = defTok; end
    tokField = uieditfield(fig, 'text', 'Position', [437, 735, 150, 22], 'Value', tok0, ...
        'Placeholder', '16 characters max');
    uilabel(fig, 'Position', [605, 735, 50, 22], 'Text', 'History:');
    histDrop = uidropdown(fig, 'Position', [655, 735, 115, 22], 'Items', histItems, ...
        'Value', prefItem('history', histItems, 'Last 10 min'));
    uilabel(fig, 'Position', [785, 735, 50, 22], 'Text', 'FS psi:');
    ptFsField = uieditfield(fig, 'numeric', 'Position', [832, 735, 60, 22], 'Value', prefOr('ptFs', ptFsDef), ...
        'Limits', [1 Inf], 'Tooltip', 'Full scale of the PT: the y-axis shows 5 % of it at least', ...
        'ValueChangedFcn', @(src, ~) scaleChanged('ptFs', src.Value));
    uilabel(fig, 'Position', [902, 735, 45, 22], 'Text', 'FS lbf:');
    lcFsField = uieditfield(fig, 'numeric', 'Position', [945, 735, 60, 22], 'Value', prefOr('lcFs', lcFsDef), ...
        'Limits', [1 Inf], 'Tooltip', 'Full scale of the load cell: the y-axis shows 5 % of it at least', ...
        'ValueChangedFcn', @(src, ~) scaleChanged('lcFs', src.Value));
    logCheck = uicheckbox(fig, 'Position', [1025, 735, 105, 22], 'Text', 'Log to CSV', 'Value', true);

    uilabel(fig, 'Position', [20, 705, 75, 22], 'Text', 'PT (psi/V):');
    ptScaleField = uieditfield(fig, 'numeric', 'Position', [90, 705, 75, 22], 'Value', prefOr('ptScale', ptScaleDef), ...
        'ValueChangedFcn', @(src, ~) scaleChanged('ptScale', src.Value));
    uilabel(fig, 'Position', [180, 705, 120, 22], 'Text', 'LC (lbf per mV/V):');
    lcScaleField = uieditfield(fig, 'numeric', 'Position', [290, 705, 75, 22], 'Value', prefOr('lcScale', lcScaleDef), ...
        'ValueChangedFcn', @(src, ~) scaleChanged('lcScale', src.Value));
    uibutton(fig, 'Position', [380, 705, 80, 22], 'Text', 'Tare PT', 'ButtonPushedFcn', @(~, ~) onTare(1));
    uibutton(fig, 'Position', [470, 705, 80, 22], 'Text', 'Tare LC', 'ButtonPushedFcn', @(~, ~) onTare(2));
    uibutton(fig, 'Position', [560, 705, 90, 22], 'Text', 'Clear tares', 'ButtonPushedFcn', @(~, ~) onTare(0));
    uilabel(fig, 'Position', [670, 705, 75, 22], 'Text', 'Thrust unit:');
    unitDrop = uidropdown(fig, 'Position', [745, 705, 65, 22], 'Items', {'lbf', 'N'}, ...
        'Value', prefItem('forceUnit', {'lbf', 'N'}, 'lbf'), 'ValueChangedFcn', @(src, ~) unitChanged(src.Value));
    uilabel(fig, 'Position', [830, 705, 100, 22], 'Text', 'Stale after (s):');
    staleField = uieditfield(fig, 'numeric', 'Position', [925, 705, 50, 22], 'Value', 3, 'Limits', [0.5 600]);

    bannerLbl = uilabel(fig, 'Position', [20, 652, 1110, 45], 'Text', banner0, 'FontSize', 24, 'FontWeight', 'bold', ...
        'HorizontalAlignment', 'center', 'BackgroundColor', grey, 'FontColor', [1 1 1]);

    ptBig = uilabel(fig, 'Position', [20, 602, 540, 45], 'Text', 'Pressure: --', 'FontSize', 30, 'FontWeight', 'bold');
    lcBig = uilabel(fig, 'Position', [590, 602, 540, 45], 'Text', 'Thrust: --', 'FontSize', 30, 'FontWeight', 'bold');
    infoLbl = uilabel(fig, 'Position', [20, 570, 1110, 28], 'Text', '', 'FontSize', 13);

    axP = uiaxes(fig, 'Position', [20, 365, 1110, 200]);
    grid(axP, 'on'); hold(axP, 'on'); ylabel(axP, 'Pressure (psi)'); title(axP, 'Pressure (PT)');
    axL = uiaxes(fig, 'Position', [20, 160, 1110, 200]);
    grid(axL, 'on'); hold(axL, 'on'); title(axL, 'Thrust (load cell)');
    axs = [axP, axL];

    % Overview of all the data in memory: one colour row for pressure (P) and one for thrust (T). The colour is
    % |value| / full scale (light grey = quiet, red = full scale). The black box is the view. A click moves the view there.
    ovAx = uiaxes(fig, 'Position', [20, 72, 1110, 85]);
    ovImg = image(ovAx, 'XData', [0 1], 'YData', [1 2], 'CData', NaN(2, 2), 'CDataMapping', 'scaled', ...
        'AlphaData', zeros(2, 2), 'ButtonDownFcn', @onOverviewClick);
    hold(ovAx, 'on');
    ovBox = plot(ovAx, NaN, NaN, '-', 'Color', [0 0 0], 'LineWidth', 1.5, 'PickableParts', 'none');
    colormap(ovAx, interp1([0 0.5 1], [0.92 0.92 0.92; 1 0.75 0.15; 0.75 0 0], linspace(0, 1, 256)));
    ovAx.CLim = [0 1];
    ovAx.YDir = 'reverse';
    ovAx.YLim = [0.5 2.5];
    ovAx.YTick = [1 2];
    ovAx.YTickLabel = {'P', 'T'};
    ovAx.FontSize = 9;
    ovAx.Box = 'on';
    ovAx.ButtonDownFcn = @onOverviewClick;
    xlabel(ovAx, 'Time (s since Connect; old data is negative).   Colour: |value| / full scale.   Click to move the view.');
    try
        disableDefaultInteractivity(ovAx);
        ovAx.Toolbar.Visible = 'off';
    catch
    end

    % View position (right end = follow the newest data) and view width (log scale)
    uilabel(fig, 'Position', [20, 38, 40, 22], 'Text', 'View:');
    posSlider = uislider(fig, 'Position', [65, 48, 500, 3], 'Limits', [0 1], 'Value', 1, 'MajorTicks', [], ...
        'MinorTicks', [], 'ValueChangingFcn', @(~, evt) onPos(evt.Value, false), ...
        'ValueChangedFcn', @(src, ~) onPos(src.Value, true));
    posLbl = uilabel(fig, 'Position', [580, 38, 230, 22], 'Text', 'LIVE (follows the newest data)', 'FontWeight', 'bold');
    widthS = min(widthMaxS, max(widthMinS, prefOr('widthS', 120)));
    widthLbl = uilabel(fig, 'Position', [815, 38, 95, 22], 'Text', ['Width: ', fmtDur(widthS)]);
    widthSlider = uislider(fig, 'Position', [915, 48, 210, 3], 'Limits', log10([widthMinS widthMaxS]), ...
        'Value', log10(widthS), 'MajorTicks', log10([5 60 600 3600 14400]), ...
        'MajorTickLabels', {'5 s', '1 min', '10 min', '1 h', '4 h'}, 'MinorTicks', [], ...
        'ValueChangingFcn', @(~, evt) onWidth(evt.Value, false), 'ValueChangedFcn', @(src, ~) onWidth(src.Value, true));
    unitChanged(unitDrop.Value);

    fig.CloseRequestFcn = @(~, ~) closeApp();

    % ===================================================================== preferences
    function v = prefOr(name, def)
        v = def;
        try
            if ispref(prefGroup, name), v = getpref(prefGroup, name); end
        catch
        end
    end

    function v = prefItem(name, items, def)
        v = prefOr(name, def);
        if ~ischar(v) || ~any(strcmp(items, v)), v = def; end
    end

    function v = tareVec(name)
        v = prefOr(name, zeros(1, nDev));
        if ~isnumeric(v) || numel(v) ~= nDev || any(~isfinite(v)), v = zeros(1, nDev); end
        v = reshape(double(v), 1, nDev);
    end

    function savePref(name, v)
        try
            setpref(prefGroup, name, v);
        catch
        end
    end

    % ===================================================================== connect, disconnect
    function onConnect(~, ~)
        disconnectAll();
        resetData();
        savePref('source', srcDrop.Value);
        if strcmp(srcDrop.Value, srcTcp)
            startServer();
        else
            connectDAQ();
        end
    end

    function resetData()
        t0 = tic;
        wallT0 = posixtime(datetime('now', 'TimeZone', 'local'));
        for k = 1:numel(hLine)
            if isgraphics(hLine(k)), delete(hLine(k)); end
        end
        hLine = gobjects(nDev, 2);
        legend(axP, 'off'); legend(axL, 'off');
        legKey = {'', ''};
        ser = repmat(ser0, nDev, 2);
        mp = repmat(mp0, 1, nDev);
        histBuf = repmat(hb0, 1, nDev);
        loadState = 'live'; loadBytes = 0; ordN = 0; nReboots = 0;
        ageMs = NaN; lastBatchT = -Inf; nBatches = 0;
        pendPT = []; pendLC = []; dtPT = 0; dtLC = 0;
        drops = NaN; rssiUp = NaN; rssiOwn = NaN; daqRun = ''; daqLink = NaN;
        upSt = struct('rssi', NaN, 'drops', NaN, 'queue', NaN, 'uptime', NaN, 'reconnects', NaN);
        upDev = NaN;
        ptBig.Text = 'Pressure: --';
        lcBig.Text = 'Thrust: --';
        dataVer = dataVer + 1; lastDraw = -Inf; ovLast = -Inf; ylCache = {[], []};
        viewLive = true; posSlider.Value = 1;
    end

    function startTimer()
        tick = 0;
        tmr = timer('ExecutionMode', 'fixedSpacing', 'Period', 0.1, 'BusyMode', 'drop', 'TimerFcn', @onTick);
        start(tmr);
    end

    function disconnectAll()
        stopTimer();
        closePort();
        closeServer();
        tcpState = 'idle';
        activeSrc = '';
        closeLog();
        statusLbl.Text = 'Status: Disconnected';
        setBanner(banner0, grey);
    end

    function openLog()
        closeLog();
        if ~logCheck.Value, return; end
        fn = fullfile(outDir, sprintf('telemetry_live_%s.csv', char(datetime('now', 'Format', 'yyyy-MM-dd_HH-mm-ss'))));
        logFile = fopen(fn, 'a');
        if logFile ~= -1
            fprintf(logFile, 'unix_time,daq,sensor,time_s,board_time_us,value,value_unit,eng,eng_unit\n');
        end
    end

    function closeLog()
        if logFile ~= -1, fclose(logFile); logFile = -1; end
    end

    function onTick(~, ~)
        tick = tick + 1;
        if strcmp(activeSrc, 'tcp')
            try
                tcpTick();
            catch ME
                if any(strcmp(tcpState, {'hello', 'run'})), dropLink(ME.message); end
            end
        elseif strcmp(activeSrc, 'usb')
            try
                if tick == 1 || mod(tick, 50) == 0       % the mode of the DAQ and the state of its own WiFi, at the start and every 5 s
                    writeline(s, "WIFI GET");
                end
                usbRead();
            catch ME
                if isempty(s) || ~isvalid(s) || contains(ME.identifier, 'ReadError') || contains(lower(ME.message), 'invalid')
                    statusLbl.Text = 'Status: Disconnected (connection to the DAQ lost)';
                    stopTimer();
                    closePort();
                    closeLog();
                    activeSrc = '';
                end
            end
        end
        try
            updateScreen(false);
        catch
        end
    end

    % ===================================================================== source: server (TCP)
    function startServer()
        host = strtrim(hostField.Value);
        tok = strtrim(tokField.Value);
        if isempty(host)
            statusLbl.Text = 'Status: type the host of the server';
            return;
        end
        if isempty(tok) || numel(tok) > 16
            statusLbl.Text = 'Status: the token must have 1 to 16 characters';
            return;
        end
        savePref('host', host);
        savePref('port', portField.Value);
        savePref('history', histDrop.Value);
        srvHost = host;
        srvPort = portField.Value;
        srvTok = zeros(1, 16, 'uint8');
        srvTok(1:numel(tok)) = uint8(tok);
        backoffS = 1; hadLink = false; srvReconnects = 0; cursor = TP_CURSOR_NOW; tcpMsg = '';
        loadState = 'start';
        activeSrc = 'tcp';
        statusLbl.Text = sprintf('Status: server %s:%d', srvHost, srvPort);
        openLog();
        setBanner(sprintf('Connecting to %s:%d ...', srvHost, srvPort), grey);
        drawnow;
        openServer();
        startTimer();
    end

    % Opens the TCP connection and sends HELLO. The answer comes in tcpTick. It blocks for 3 s at most.
    function openServer()
        closeServer();
        try
            tc = tcpclient(srvHost, srvPort, 'ConnectTimeout', 3, 'Timeout', 1);
            write(tc, tpFrame(TP_HELLO, [uint8(TP_ROLE_DOWNLOAD), srvTok, zeros(1, 8, 'uint8')]), "uint8");
            tcpState = 'hello';
            stateT = toc(t0);
        catch ME
            dropLink(['cannot connect: ', ME.message]);
        end
    end

    function closeServer()
        if ~isempty(tc)
            try
                delete(tc);
            catch
            end
        end
        tc = [];
        rx = zeros(1, 0, 'uint8');
        getOut = false;
    end

    % Closes the connection and connects again later (1 s, 2 s, 4 s ... 10 s). The cursor stays: the monitor continues
    % where it stopped. A frame-rate probe starts again.
    function dropLink(msg)
        closeServer();
        tcpState = 'wait';
        tcpMsg = msg;
        retryAt = toc(t0) + backoffS;
        backoffS = min(backoffS * 2, 10);
        if strcmp(loadState, 'probe'), loadState = 'start'; cursor = TP_CURSOR_NOW; end
    end

    % One tick of the link. While the server has more data (a full BATCH: old data or a catch-up), it asks again at
    % once, for 0.3 s at most, so a history load is not limited by the 0.1 s timer.
    function tcpTick()
        tEnd = toc(t0) + 0.3;
        while true
            tcpPoll();
            if ~moreWaiting || ~strcmp(tcpState, 'run') || toc(t0) > tEnd, break; end
            while tc.NumBytesAvailable == 0 && toc(t0) < tEnd
                pause(0.002);
            end
        end
    end

    function tcpPoll()
        moreWaiting = false;
        tNow = toc(t0);
        if strcmp(tcpState, 'wait')
            if tNow >= retryAt, openServer(); end
            return;
        end
        if ~any(strcmp(tcpState, {'hello', 'run'})), return; end
        n = tc.NumBytesAvailable;
        if n > 0
            rx = [rx, reshape(uint8(read(tc, n, "uint8")), 1, [])];
        end
        pos = 1;
        while true
            [st, type, pay, nxt] = tpNext(rx, pos, true);
            if st == 0, break; end                   % a frame is not complete yet
            pos = nxt;
            if st < 0, continue; end                 % junk or a bad CRC: skip to the next sync byte
            onFrame(type, pay);
            if ~any(strcmp(tcpState, {'hello', 'run'})), return; end     % the connection was closed (rx is empty now)
        end
        rx = rx(pos:end);
        if strcmp(tcpState, 'hello') && tNow - stateT > 5
            dropLink('no answer to HELLO');
        elseif strcmp(tcpState, 'run') && getOut && tNow - getT > getWaitMs / 1000 + 4
            dropLink('no BATCH from the server');
        end
    end

    function onFrame(type, pay)
        if strcmp(tcpState, 'hello')
            if type == TP_ACK && numel(pay) == 8
                tcpState = 'run';
                if hadLink, srvReconnects = srvReconnects + 1; end
                hadLink = true;
                backoffS = 1;
                tcpMsg = '';
                if strcmp(loadState, 'start'), beginHistory(double(typecast(pay(1:8), 'uint64'))); end
                sendGet();
            elseif type == TP_NAK
                reason = 0;
                if ~isempty(pay), reason = double(pay(1)); end
                if reason == TP_NAK_TOKEN
                    closeServer();
                    tcpState = 'refused';
                    tcpMsg = 'wrong token';
                else
                    dropLink(sprintf('NAK %d to HELLO', reason));
                end
            else
                dropLink(sprintf('frame type %d instead of ACK', type));
            end
        else
            if type == TP_BATCH && numel(pay) >= 12
                getOut = false;
                handleBatch(pay);
                sendGet();
            elseif type == TP_RESET && numel(pay) >= 8
                serverWasReset(char(pay(9:end)));
            elseif type == TP_NAK
                if toc(t0) - resetSentT < 10
                    resetSentT = -Inf;
                    dropLink('the server does not know RESET: redeploy the new server');
                    statusLbl.Text = 'Status: RESET refused: the server is an old version (redeploy it from git)';
                else
                    dropLink('NAK from the server');
                end
            end
        end
    end

    % "Save & clear server": the server closes its archive files (they stay on the server), starts new ones, and empties
    % its ring. The monitor then clears its own data too. The cursor continues (the server keeps counting frames).
    function onServerReset(~, ~)
        if ~strcmp(activeSrc, 'tcp') || ~strcmp(tcpState, 'run')
            uialert(fig, 'Connect to the server first (Source: Server (TCP)).', 'Save & clear server');
            return;
        end
        sel = uiconfirm(fig, sprintf(['The server closes its archive files (they stay on the server) and starts new ones.\n', ...
            'It empties its history: Connect can no longer load the old data.\n', ...
            'This monitor also clears its plots (the CSV log keeps the old data).\n\nContinue?']), 'Save & clear server', ...
            'Options', {'Save & clear', 'Cancel'}, 'DefaultOption', 2, 'CancelOption', 2, 'Icon', 'warning');
        if ~strcmp(sel, 'Save & clear') || ~strcmp(tcpState, 'run'), return; end
        try
            write(tc, tpFrame(TP_RESET, zeros(1, 0, 'uint8')), "uint8");
            resetSentT = toc(t0);
            statusLbl.Text = 'Status: RESET sent to the server...';
        catch ME
            dropLink(ME.message);
        end
    end

    function serverWasReset(archiveName)
        resetData();                                 % new time base: the times of the link start again too
        getT = toc(t0); stateT = getT; resetSentT = -Inf;
        openLog();
        if isempty(archiveName), archiveName = '(the server has no archive folder)'; end
        statusLbl.Text = ['Status: server cleared, new archive ', archiveName];
        redrawSoon();
    end

    function sendGet()
        g = [typecast(uint64(cursor), 'uint8'), typecast(uint16(getBytes), 'uint8'), typecast(uint16(getWaitMs), 'uint8')];
        write(tc, tpFrame(TP_GET, g), "uint8");
        getOut = true;
        getT = toc(t0);
    end

    % ===================================================================== history load
    % The first ACK after Connect. head = the index of the next frame on the server.
    function beginHistory(head)
        hs = histSecs(strcmp(histItems, histDrop.Value));
        if isempty(hs) || hs == 0 || head == 0
            cursor = TP_CURSOR_NOW;
            loadState = 'live';
        elseif isinf(hs) || head <= probeFrames
            startLoad(0, head);
        else
            loadTo = head;
            cursor = uint64(head - probeFrames);
            loadState = 'probe';
        end
    end

    function startLoad(c0, head)
        cursor = uint64(c0);
        loadFrom = c0; loadTo = head; loadBytes = 0; ordN = 0; nReboots = 0;
        histBuf = repmat(hb0, 1, nDev);
        loadState = 'loading';
    end

    % The answer to the probe: frames per second of data, from the board clocks of the frames. Then the load starts that
    % many frames before the head.
    function probeDone(nFrames, usAll)
        span = 0;
        for d = 1:nDev
            raw = usAll{d};
            if numel(raw) < 2, continue; end
            w = mod(raw - raw(1) + 2^31, 2^32) - 2^31;
            span = max(span, (max(w) - min(w)) / 1e6);
        end
        hs = histSecs(strcmp(histItems, histDrop.Value));
        if span >= 1 && nFrames > 0
            c0 = max(0, loadTo - ceil(nFrames / span * hs * 1.05));
        else
            c0 = 0;                                  % (almost) no data in the last frames: load all (it is small)
        end
        startLoad(c0, loadTo);
    end

    function histAdd(d, sn, ord, us, v)
        m = numel(us);
        if m == 0, return; end
        n = histBuf(d).n;
        if n + m > numel(histBuf(d).raw)
            cap = max(2 * numel(histBuf(d).raw), n + m + 4096);
            histBuf(d).raw(cap) = 0; histBuf(d).v(cap) = 0; histBuf(d).s(cap) = 0; histBuf(d).ord(cap) = 0;
        end
        histBuf(d).raw(n+1:n+m) = us;
        histBuf(d).v(n+1:n+m) = v;
        histBuf(d).s(n+1:n+m) = sn;
        histBuf(d).ord(n+1:n+m) = ord;
        histBuf(d).n = n + m;
    end

    % The load reached the newest data. The newest record maps to "now - data age". Each board clock is unwrapped (u32,
    % 71.6 min). A jump of more than 30 s is a reboot of that board: the older part ends 0.1 s before it. The DAQ with the
    % newest frame is the reference. Each other DAQ lines up with the reference at the frame index of its own last frame.
    function finishHistory(tArr)
        ids = find([histBuf.n] > 0);
        if ~isempty(ids)
            U = cell(1, nDev);
            lastOrd = zeros(1, nDev);
            for d = ids
                n = histBuf(d).n;
                du = mod(diff(histBuf(d).raw(1:n)) + 2^31, 2^32) - 2^31;
                br = abs(du) > 30e6;
                nReboots = nReboots + nnz(br);
                du(br) = 1e5;
                U{d} = [0, cumsum(du)];
                lastOrd(d) = histBuf(d).ord(n);
            end
            [~, ref] = max(lastOrd);
            if isnan(ageMs), target = tArr; else, target = tArr - ageMs / 1000; end
            off = zeros(1, nDev);
            off(ref) = target - U{ref}(end) / 1e6;
            [fo, ia] = unique(histBuf(ref).ord(1:histBuf(ref).n), 'last');
            ft = U{ref}(ia) / 1e6 + off(ref);
            for d = ids
                if d == ref, continue; end
                o = lastOrd(d);
                tr = NaN;
                if numel(fo) > 1, tr = interp1(fo, ft, o, 'linear', NaN); end
                if isnan(tr)
                    if o <= fo(1), tr = ft(1); else, tr = ft(end); end
                end
                off(d) = tr - U{d}(end) / 1e6;
            end
            for d = ids
                n = histBuf(d).n;
                t = U{d} / 1e6 + off(d);
                mp(d).haveMap = true;
                mp(d).rawRef = histBuf(d).raw(n);
                mp(d).uRef = U{d}(end);
                mp(d).offS = off(d);
                for sn = 1:2
                    k = histBuf(d).s(1:n) == sn;
                    if any(k)
                        raw = histBuf(d).raw(1:n); v = histBuf(d).v(1:n);
                        addSamples(d, sn, t(k), v(k), raw(k));
                    end
                end
            end
        end
        histBuf = repmat(hb0, 1, nDev);
        loadState = 'live';
    end

    % ===================================================================== BATCH
    % One BATCH: next cursor u64, age_ms u32, then whole stored frames (the same as handle_batch() in src/telem.c).
    % The frames of several DAQs come mixed. The src_dev of a frame is the device ID of the board that made the data.
    function handleBatch(pay)
        tArr = toc(t0);
        nxt = typecast(pay(1:8), 'uint64');
        age = double(typecast(pay(9:12), 'uint32'));
        if age == TP_AGE_NONE, ageMs = NaN; else, ageMs = age; end
        full = numel(pay) > getBytes / 2;            % the server has more data after this batch
        loading = strcmp(loadState, 'loading');
        usPT = cell(1, nDev); vPT = cell(1, nDev); usLC = cell(1, nDev); vLC = cell(1, nDev);
        nFrames = 0;
        pos = 13;
        while pos <= numel(pay)
            [st, type, p, nx, src] = tpNext(pay, pos, false);     % the CRC of the BATCH covers these bytes already
            if st ~= 1, break; end
            pos = nx;
            nFrames = nFrames + 1;
            d = src + 1;
            if type == TP_STATUS && numel(p) == 16
                upDev = src;
                upSt.rssi = double(typecast(p(1), 'int8'));
                upSt.drops = double(typecast(p(3:6), 'uint32'));
                upSt.queue = double(typecast(p(7:10), 'uint32'));
                upSt.uptime = double(typecast(p(11:14), 'uint32'));
                upSt.reconnects = double(typecast(p(15:16), 'uint16'));
            elseif d > nDev
                continue;                                             % not a DAQ (a test frame): ignore
            elseif type == TP_DATA_PT || type == TP_DATA_LC
                [us, rd, sq] = decodeRecs(p);
                if type == TP_DATA_PT
                    k = us < META_BASE & bitand(sq, 32768) == 0;
                    sn = 1; v = double(rd(k)) * voltPerLsb;
                else
                    k = us < META_BASE & bitand(sq, 32768) ~= 0;
                    sn = 2; v = lcCount(rd(k), sq(k)) * lcMvVPerCount;
                end
                us = us(k);
                if loading
                    ordN = ordN + 1;
                    histAdd(d, sn, ordN, us, v);
                elseif sn == 1
                    usPT{d} = [usPT{d}, us]; vPT{d} = [vPT{d}, v];
                else
                    usLC{d} = [usLC{d}, us]; vLC{d} = [vLC{d}, v];
                end
            end
        end
        markBatch(tArr);
        if strcmp(loadState, 'probe')
            probeDone(nFrames, cellfun(@(a, b) [a, b], usPT, usLC, 'UniformOutput', false));
            moreWaiting = true;
            return;
        end
        cursor = nxt;
        moreWaiting = full;
        if loading
            loadBytes = loadBytes + numel(pay);
            if ~full, finishHistory(tArr); end
            return;
        end
        for d = 1:nDev
            if isempty(usPT{d}) && isempty(usLC{d}), continue; end
            t = mapTimes(d, [usPT{d}, usLC{d}], tArr, full);
            nP = numel(usPT{d});
            addSamples(d, 1, t(1:nP), vPT{d}, usPT{d});
            addSamples(d, 2, t(nP+1:end), vLC{d}, usLC{d});
        end
    end

    function markBatch(tNow)
        nBatches = nBatches + 1;
        lastBatchT = tNow;
    end

    % The board clock of DAQ d (u32 us, wraps after 71.6 min) to the time of this monitor. The newest record of a batch is
    % about "now - data age". The map follows slowly (jitter of the network), and jumps when the error is above 2 s
    % (a reboot of the DAQ). A batch that catches up (a full one) is not the newest data: it does not move the map.
    function t = mapTimes(d, raw, tArr, catchingUp)
        m = mp(d);
        if ~m.haveMap, m.rawRef = raw(end); m.uRef = 0; end
        u = m.uRef + mod(raw - m.rawRef + 2^31, 2^32) - 2^31;
        [uNew, iNew] = max(u);
        if isnan(ageMs), target = tArr; else, target = tArr - ageMs / 1000; end
        err = target - (uNew / 1e6 + m.offS);
        if ~m.haveMap || abs(err) > 2
            if ~m.haveMap || ~catchingUp
                m.offS = target - uNew / 1e6;
                m.haveMap = true;
            end
        elseif ~catchingUp
            m.offS = m.offS + 0.05 * err;
        end
        m.rawRef = raw(iNew);
        m.uRef = uNew;
        t = u / 1e6 + m.offS;
        mp(d) = m;
    end

    % ===================================================================== source: DAQ on USB
    function connectDAQ()
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
                        activeSrc = 'usb';
                        openLog();
                        startTimer();
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

    % One read of the serial port: every complete line is one of the line types of the header of this file.
    function usbRead()
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
                usbCommit(toc(t0));
            elseif startsWith(ln, 'WIFI:OK')
                tok = regexp(ln, 'run=(\w+).*?link=(\d)', 'tokens', 'once');
                if ~isempty(tok), daqRun = tok{1}; daqLink = str2double(tok{2}); end
            end
        end
    end

    function usbCommit(tNow)
        markBatch(tNow);
        if ~isempty(pendPT)
            m = numel(pendPT);
            dt = dtPT; if dt <= 0, dt = 0.5 / m; end
            addSamples(1, 1, tNow - (m-1:-1:0) * dt, pendPT, []);        % a DAQ on USB is index 1
        end
        if ~isempty(pendLC)
            m = numel(pendLC);
            dt = dtLC; if dt <= 0, dt = 0.5 / m; end
            addSamples(1, 2, tNow - (m-1:-1:0) * dt, pendLC * lcMvVPerCount, []);
        end
        pendPT = []; pendLC = [];
    end

    % ===================================================================== samples (both sources)
    % Adds samples of DAQ d, sensor sn (PT in volts, load cell in mV/V). us = the board time (empty from USB).
    % A gap of more than 4 sample intervals (0.5 s at least) gets a NaN point, so the line breaks there.
    % The buffers grow by doubling and are written in place, so a long history load stays fast.
    function addSamples(d, sn, t, v, us)
        m = numel(t);
        if m == 0, return; end
        n = ser(d, sn).n;
        if m > 1
            ser(d, sn).dt = median(diff(t));
        elseif ser(d, sn).dt == 0 && n > 0
            ser(d, sn).dt = t(1) - ser(d, sn).T(n);
        end
        logRows(d, sn, t, us, v);
        thr = max(0.5, 4 * ser(d, sn).dt);
        [t, v] = insertGaps(t, v, thr);
        if n > 0 && t(1) - ser(d, sn).T(n) > thr
            t = [ser(d, sn).T(n), t];
            v = [NaN, v];
        end
        m = numel(t);
        if n + m > numel(ser(d, sn).T)
            cap = max(2 * numel(ser(d, sn).T), n + m + 4096);
            ser(d, sn).T(cap) = 0;
            ser(d, sn).V(cap) = 0;
        end
        ser(d, sn).T(n+1:n+m) = t;
        ser(d, sn).V(n+1:n+m) = v;
        ser(d, sn).n = n + m;
        dataVer = dataVer + 1;
    end

    function logRows(d, sn, t, us, v)
        if logFile == -1, return; end
        if sn == 1, nm = 'PT'; bu = 'V'; eu = 'psi'; else, nm = 'LC'; bu = 'mV/V'; eu = 'lbf'; end
        if strcmp(activeSrc, 'usb'), who = 'USB'; else, who = sprintf('%d', d - 1); end
        e = engOf(d, sn, v);
        if isempty(us)
            fprintf(logFile, ['%.3f,', who, ',', nm, ',%.4f,,%.9g,', bu, ',%.9g,', eu, '\n'], [wallT0 + t; t; v; e]);
        else
            fprintf(logFile, ['%.3f,', who, ',', nm, ',%.4f,%.0f,%.9g,', bu, ',%.9g,', eu, '\n'], [wallT0 + t; t; us; v; e]);
        end
    end

    % volts -> psi, mV/V -> lbf, with the tare of DAQ d
    function y = engOf(d, sn, v)
        if sn == 1
            y = ptScaleField.Value * v - ptTare(d);
        else
            y = lcScaleField.Value * v - lcTare(d);
        end
    end

    function [k, u] = forceScale()
        if strcmp(unitDrop.Value, 'N'), k = lbfToN; u = 'N'; else, k = 1; u = 'lbf'; end
    end

    function n = dname(d)
        if strcmp(activeSrc, 'usb'), n = 'DAQ (USB)'; else, n = sprintf('DAQ %d', d - 1); end
    end

    % ===================================================================== tare, scale, unit
    % sn = 1: PT, 2: load cell, 0: clear both. Each DAQ gets the mean of its own last 2 s.
    function onTare(sn)
        if sn == 0
            ptTare = zeros(1, nDev); lcTare = zeros(1, nDev);
        else
            ids = find([ser(:, sn).n] > 0);
            if isempty(ids)
                statusLbl.Text = 'Status: no data to tare';
                return;
            end
            for d = ids
                n = ser(d, sn).n;
                T = ser(d, sn).T(1:n); V = ser(d, sn).V(1:n);
                k = T >= T(n) - tareS & ~isnan(V);
                if ~any(k), continue; end
                if sn == 1
                    ptTare(d) = ptScaleField.Value * mean(V(k));
                else
                    lcTare(d) = lcScaleField.Value * mean(V(k));
                end
            end
        end
        savePref('ptTare', ptTare);
        savePref('lcTare', lcTare);
        redrawSoon();
    end

    function scaleChanged(name, v)
        savePref(name, v);
        redrawSoon();
    end

    function unitChanged(u)
        savePref('forceUnit', u);
        lbl = ['Thrust (', u, ')'];
        if ~strcmp(lbl, lastLblL), ylabel(axL, lbl); lastLblL = lbl; end
        redrawSoon();
    end

    function redrawSoon()
        dataVer = dataVer + 1;
        lastDraw = -Inf;
        ovLast = -Inf;
        try
            updateScreen(true);
        catch
        end
    end

    % ===================================================================== screen
    function ensureLine(sn, d)
        if ~isgraphics(hLine(d, sn))
            hLine(d, sn) = plot(axs(sn), NaN, NaN, '-', 'Color', pal(d, :), 'LineWidth', 1.2);
        end
    end

    function tA = oldestT()
        tA = Inf;
        for sn = 1:2
            for d = find([ser(:, sn).n] > 0)
                tA = min(tA, ser(d, sn).T(1));
            end
        end
    end

    % The View slider: 0 = the view ends one width after the oldest data, 1 = live.
    function lo = posLow(tNow)
        tA = oldestT();
        if isinf(tA), lo = tNow; else, lo = min(tA + widthS, tNow); end
    end

    function onPos(v, final)
        tNow = toc(t0);
        lastDragT = tNow;
        if v >= 0.995
            viewLive = true;
        else
            lo = posLow(tNow);
            viewLive = false;
            viewEnd = lo + v * (tNow - lo);
        end
        viewChanged(final);
    end

    function onWidth(v, final)
        widthS = 10 ^ v;
        setText(widthLbl, ['Width: ', fmtDur(widthS)]);
        if final, savePref('widthS', widthS); end
        viewChanged(final);
    end

    function onOverviewClick(~, ~)
        x = ovAx.CurrentPoint(1, 1);
        tNow = toc(t0);
        if x + widthS / 2 >= tNow - 0.5
            viewLive = true;
            posSlider.Value = 1;
        else
            viewLive = false;
            viewEnd = x + widthS / 2;
            lastDragT = -Inf;                        % updateScreen moves the slider to the new place
        end
        viewChanged(true);
    end

    % While a slider moves, draw at the normal rate (4 Hz); at the end, draw at once.
    function viewChanged(final)
        dataVer = dataVer + 1;
        if final
            redrawSoon();
        else
            try
                updateScreen(false);
            catch
            end
        end
    end

    % The y-axis spans 5 % of the full scale at least, so the noise of a quiet sensor stays small.
    function setYLim(sn, lo, hi, fs)
        if ~isfinite(lo) || ~isfinite(hi), return; end
        minSpan = minSpanFrac * abs(fs);
        span = hi - lo;
        if span < minSpan
            yl = (lo + hi) / 2 + [-0.5 0.5] * minSpan;
        else
            yl = [lo, hi] + [-0.05 0.05] * span;
        end
        if ~isequal(yl, ylCache{sn})
            ylim(axs(sn), yl);
            ylCache{sn} = yl;
        end
    end

    % The colour rows of the overview: once per second (or after a change of the scales). The view box: at each draw.
    function updateOverview(tNow, tA, x0, x1)
        if isinf(tA)
            set(ovBox, 'XData', NaN, 'YData', NaN);
            set(ovImg, 'AlphaData', zeros(2, 2), 'CData', NaN(2, 2));
            return;
        end
        tB = max(tNow, tA + 1);
        if tNow - ovLast >= 1
            ovLast = tNow;
            nb = 600;
            A = NaN(2, nb);
            fs = [ptFsField.Value, lcFsField.Value];
            for sn = 1:2
                for d = find([ser(:, sn).n] > 0)
                    n = ser(d, sn).n;
                    T = ser(d, sn).T(1:n); v = ser(d, sn).V(1:n);
                    k = ~isnan(v);
                    if ~any(k), continue; end
                    b = min(nb, max(1, floor((T(k) - tA) / (tB - tA) * nb) + 1));
                    a = accumarray(b(:), reshape(abs(engOf(d, sn, v(k))), [], 1) / fs(sn), [nb 1], @max, NaN);
                    A(sn, :) = max(A(sn, :), a');
                end
            end
            bw = (tB - tA) / nb;
            set(ovImg, 'XData', [tA + bw / 2, tB - bw / 2], 'CData', min(A, 1), 'AlphaData', double(~isnan(A)));
            xlim(ovAx, [tA, tB]);
        end
        set(ovBox, 'XData', [x0 x1 x1 x0 x0], 'YData', [0.55 0.55 2.45 2.45 0.55]);
    end

    function updateScreen(force)
        tNow = toc(t0);
        if ~force && tNow - lastDraw < drawPeriod, return; end
        lastDraw = tNow;
        [fK, fU] = forceScale();
        units = {'psi', fU};
        fsU = [ptFsField.Value, lcFsField.Value * fK];       % the full scales in the units of the plots
        bigs = [ptBig, lcBig];
        bigName = {'Pressure', 'Thrust'};
        tA = oldestT();
        if viewLive
            x1 = tNow;
        else
            viewEnd = min(viewEnd, tNow);
            x1 = viewEnd;
        end
        x0 = x1 - widthS;
        if ~isinf(tA), x0 = max(x0, tA); end              % a view wider than the data starts at the oldest data
        if x1 - x0 < 1, x0 = x1 - 1; end
        for sn = 1:2
            ids = find([ser(:, sn).n] > 0);
            if dataVer == drawnVer || isempty(ids), continue; end
            txt = cell(1, numel(ids));
            lo = Inf; hi = -Inf;
            for i = 1:numel(ids)
                d = ids(i);
                ensureLine(sn, d);
                n = ser(d, sn).n;
                T = ser(d, sn).T(1:n);
                i1 = find(T >= x0, 1);
                i2 = find(T <= x1, 1, 'last');
                if isempty(i1) || isempty(i2) || i2 < i1
                    set(hLine(d, sn), 'XData', NaN, 'YData', NaN);
                else
                    y = engOf(d, sn, ser(d, sn).V(i1:i2));
                    if sn == 2, y = y * fK; end
                    [xp, yp] = minMaxDecimate(T(i1:i2), y, maxPts);
                    set(hLine(d, sn), 'XData', xp, 'YData', yp);
                    lo = min(lo, min(yp)); hi = max(hi, max(yp));
                end
                vLast = engOf(d, sn, ser(d, sn).V(n));
                if sn == 2, vLast = vLast * fK; end
                txt{i} = sprintf('%s %.1f %s', dname(d), vLast, units{sn});
            end
            setText(bigs(sn), [bigName{sn}, ': ', strjoin(txt, '   ')]);
            fs = max(14, 30 - 8 * (numel(ids) - 1));
            if bigs(sn).FontSize ~= fs, bigs(sn).FontSize = fs; end
            key = sprintf('%d,', ids);
            if ~strcmp(key, legKey{sn})
                legend(axs(sn), hLine(ids, sn), arrayfun(@dname, ids, 'UniformOutput', false), 'Location', 'northwest');
                legKey{sn} = key;
            end
            setYLim(sn, lo, hi, fsU(sn));
        end
        drawnVer = dataVer;
        xlim(axP, [x0, x1]);
        xlim(axL, [x0, x1]);
        updateOverview(tNow, tA, x0, x1);
        if viewLive
            setText(posLbl, 'LIVE (follows the newest data)');
        else
            setText(posLbl, sprintf('View ends %s before now', fmtDur(tNow - x1)));
            if tNow - lastDragT > 1                  % the newest data moves on: the slider follows (not while it is dragged)
                lo = posLow(tNow);
                v = max(0, min(1, (x1 - lo) / max(tNow - lo, eps)));
                if abs(posSlider.Value - v) > 0.003, posSlider.Value = min(v, 0.99); end
            end
        end

        % link state and banner
        if ~isempty(activeSrc)
            updateBanner(tNow);
        end
        drawnow limitrate;
    end

    function updateBanner(tNow)
        sinceBatch = tNow - lastBatchT;
        limitS = staleField.Value;
        tcp = strcmp(activeSrc, 'tcp');
        if tcp
            noBatchS = getWaitMs / 1000 + 3;             % the server answers every 2 s at least
        else
            noBatchS = 8;                                % the server answers the DAQ every 5 s at least
        end
        if tcp && strcmp(tcpState, 'refused')
            setBanner('The server refused the token. Check it and push Connect.', red);
        elseif ~tcp && ~isempty(daqRun) && ~strcmp(daqRun, 'DOWNLOAD')
            setBanner(sprintf('This DAQ is not in DOWNLOAD mode (run=%s). The data is not from the rocket.', daqRun), orange);
        elseif tcp && ~strcmp(tcpState, 'run') && (nBatches == 0 || ~strcmp(loadState, 'live'))
            setBanner(sprintf('Connecting to %s:%d ... %s', srvHost, srvPort, tcpMsg), grey);
        elseif tcp && strcmp(loadState, 'probe')
            setBanner('Finding the start of the history on the server...', blue);
        elseif tcp && strcmp(loadState, 'loading')
            p = (double(cursor) - loadFrom) / max(1, loadTo - loadFrom);
            setBanner(sprintf('Loading history from the server: %.0f %%  (%.2f MB)', 100 * min(1, p), loadBytes / 1e6), blue);
        elseif nBatches == 0
            setBanner('Waiting for the first batch from the server...', grey);
        elseif sinceBatch > noBatchS
            setBanner(sprintf('NO LINK to the server: no data for %.0f s', sinceBatch), red);
        elseif isnan(ageMs)
            setBanner('Server has no data from the rocket yet', red);
        elseif ageMs / 1000 > limitS
            setBanner(sprintf('STALE: the newest data is %.1f s old', ageMs / 1000), red);
        else
            setBanner(sprintf('LIVE   data age %.1f s', ageMs / 1000), green);
        end
        if tcp
            t = sprintf(['Upload DAQ%s: RSSI %s dBm, drops %s, queue %s, up %s s, reconnects %s   |   ', ...
                'Server link: %s, reconnects %d   |   batches %d   |   tare: %s'], upName(), rssiText(upSt.rssi), ...
                num2strOrDash(upSt.drops), num2strOrDash(upSt.queue), num2strOrDash(upSt.uptime), ...
                num2strOrDash(upSt.reconnects), serverText(), srvReconnects, nBatches, tareText());
            if nReboots > 0, t = sprintf('%s   |   %d reboot(s) in the history', t, nReboots); end
        else
            t = sprintf(['Upload DAQ: drops %s, RSSI %s dBm   |   This DAQ: RSSI %s dBm, link to server %s   |   ', ...
                'batches %d   |   tare: %s'], num2strOrDash(drops), num2strOrDash(rssiUp), num2strOrDash(rssiOwn), ...
                linkText(), nBatches, tareText());
        end
        setText(infoLbl, t);
    end

    % A uifigure redraws a label each time a property is set, also with the same value: set only changes.
    function setText(h, txt)
        if ~strcmp(h.Text, txt), h.Text = txt; end
    end

    function setBanner(txt, color)
        setText(bannerLbl, txt);
        if ~isequal(bannerLbl.BackgroundColor, color), bannerLbl.BackgroundColor = color; end
    end

    function t = num2strOrDash(v)
        if isnan(v), t = '--'; else, t = sprintf('%g', v); end
    end

    function t = rssiText(v)
        if isnan(v) || v == 0, t = '--'; else, t = sprintf('%g', v); end      % 0 = not known
    end

    function t = linkText()
        if isnan(daqLink), t = '--'; elseif daqLink ~= 0, t = 'up'; else, t = 'DOWN'; end
    end

    function t = serverText()
        switch tcpState
            case 'run', t = 'up';
            case 'hello', t = 'login';
            case 'refused', t = 'wrong token';
            otherwise
                if isempty(tcpMsg), t = 'DOWN'; else, t = ['DOWN (', tcpMsg, ')']; end
        end
    end

    function t = upName()
        if isnan(upDev), t = ''; else, t = sprintf(' (ID %d)', upDev); end
    end

    function t = tareText()
        ids = find([ser(:, 1).n] > 0 | [ser(:, 2).n] > 0);
        if isempty(ids), t = '--'; return; end
        parts = cell(1, numel(ids));
        for i = 1:numel(ids)
            d = ids(i);
            parts{i} = sprintf('%s PT %.2f psi, LC %.2f lbf', dname(d), ptTare(d), lcTare(d));
        end
        t = strjoin(parts, '; ');
    end

    % ===================================================================== motor performance
    function openPerf(~, ~)
        if isempty(perfFig) || ~isvalid(perfFig)
            perfFig = uifigure('Name', 'Motor performance', 'Position', [140, 60, 1050, 720]);
            uibutton(perfFig, 'Position', [20, 688, 130, 24], 'Text', 'Analyze again', 'ButtonPushedFcn', @(~, ~) runPerf());
            uibutton(perfFig, 'Position', [160, 688, 130, 24], 'Text', 'Save results', 'ButtonPushedFcn', @(~, ~) savePerf());
            perfLbl = uilabel(perfFig, 'Position', [305, 688, 725, 24], 'Text', '');
            perfAxT = uiaxes(perfFig, 'Position', [20, 360, 640, 320]);
            perfAxP = uiaxes(perfFig, 'Position', [20, 20, 640, 330]);
            perfTxt = uitextarea(perfFig, 'Position', [675, 20, 355, 660], 'Editable', 'off', 'FontName', 'Courier New');
        end
        figure(perfFig);
        runPerf();
    end

    % The strongest peak of sensor sn among the DAQs. Thrust in N, pressure in psi (with the tare; the analysis
    % removes the baseline before the burn).
    function [dBest, rBest, msgs] = bestPeak(sn)
        dBest = 0; rBest = burnStats([], [], 1); msgs = {};
        minAmp = 5;                                          % N or psi: a smaller peak is not a motor burn
        ids = find([ser(:, sn).n] > 0);
        if isempty(ids), msgs = {'no data of this sensor'}; end
        for d = ids
            n = ser(d, sn).n;
            t = ser(d, sn).T(1:n); v = ser(d, sn).V(1:n);
            k = ~isnan(v);
            [t, i] = sort(t(k)); y = engOf(d, sn, v(k)); y = y(i);
            if sn == 2, y = y * lbfToN; end
            r = burnStats(t, y, minAmp);
            if r.found
                if ~rBest.found || r.strength > rBest.strength
                    dBest = d; rBest = r;
                    rBest.tAll = t; rBest.yAll = y - r.base;
                end
            else
                msgs{end+1} = sprintf('%s: %s', dname(d), r.msg);       %#ok<AGROW>
            end
        end
    end

    function runPerf()
        if ~isvalid(perfFig), return; end
        [dF, rF, mF] = bestPeak(2);
        [dP, rP, mP] = bestPeak(1);
        if rF.found, tRef = rF.tStart; elseif rP.found, tRef = rP.tStart; else, tRef = 0; end
        L = {sprintf('Motor performance  %s', char(datetime('now', 'Format', 'yyyy-MM-dd HH:mm:ss'))), ...
            'Data: all the telemetry in memory', ''};
        f = '%-24s %s';
        if rF.found
            I = rF.integral;
            [cls, pct] = motorClass(I);
            L = [L, {sprintf('THRUST  (%s, load cell)', dname(dF)), ...
                sprintf(f, 'Burn start', sprintf('%.3f s', rF.tStart)), ...
                sprintf(f, 'Burn end', sprintf('%.3f s', rF.tEnd)), ...
                sprintf(f, 'Burn time (5%-5%)', sprintf('%.3f s', rF.burnS)), ...
                sprintf(f, 'Peak (top 5% avg)', sprintf('%.1f N  (%.1f lbf)', rF.top5, rF.top5 / lbfToN)), ...
                sprintf(f, 'Max sample', sprintf('%.1f N at %.3f s', rF.maxVal, rF.tMax)), ...
                sprintf(f, 'Average thrust', sprintf('%.1f N  (%.1f lbf)', rF.avg, rF.avg / lbfToN)), ...
                sprintf(f, 'Total impulse', sprintf('%.1f N s  (%.1f lbf s)', I, I / lbfToN)), ...
                sprintf(f, 'Motor class', sprintf('%s  (%.0f %% of %s)', cls, pct, cls)), ...
                sprintf(f, 'Baseline removed', sprintf('%.2f N  (%.2f lbf)', rF.base, rF.base / lbfToN)), ...
                sprintf(f, 'Peak / noise', sprintf('%.0f x', rF.strength)), ''}];
        else
            L = [L, {'THRUST: no strong peak found', mF{:}, ''}];
        end
        if rP.found
            L = [L, {sprintf('PRESSURE  (%s, PT)', dname(dP)), ...
                sprintf(f, 'Peak (top 5% avg)', sprintf('%.1f psi', rP.top5)), ...
                sprintf(f, 'Max sample', sprintf('%.1f psi at %.3f s', rP.maxVal, rP.tMax)), ...
                sprintf(f, 'Average (5%-5%)', sprintf('%.1f psi', rP.avg)), ...
                sprintf(f, 'Pressure burn time', sprintf('%.3f s (%.3f to %.3f s)', rP.burnS, rP.tStart, rP.tEnd)), ...
                sprintf(f, 'Baseline removed', sprintf('%.2f psi', rP.base)), ...
                sprintf(f, 'Peak / noise', sprintf('%.0f x', rP.strength)), ''}];
        else
            L = [L, {'PRESSURE: no strong peak found', mP{:}, ''}];
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
            if q.cutStart, L{end+1} = 'WARNING: the data starts inside the burn (load more history).'; end   %#ok<AGROW>
            if q.cutEnd, L{end+1} = 'WARNING: the data ends inside the burn.'; end                            %#ok<AGROW>
            if q.gapMax > 0.25
                L{end+1} = sprintf('WARNING: a data gap of %.2f s is inside the burn (straight line over it).', q.gapMax); %#ok<AGROW>
            end
        end
        L = [L, {'', 'Telemetry data is decimated (about 60 samples/s).', 'Use the SD or NAND file for the final numbers.'}];
        perfTxt.Value = L(:);
        plotBurn(perfAxT, dF, rF, tRef, 2);
        plotBurn(perfAxP, dP, rP, tRef, 1);
        perfRes = struct('dF', dF, 'rF', rF, 'dP', dP, 'rP', rP, 'tRef', tRef);
        perfLbl.Text = '';
    end

    function plotBurn(ax, d, r, tRef, sn)
        cla(ax); legend(ax, 'off'); hold(ax, 'on'); grid(ax, 'on');
        if sn == 2, nm = 'Thrust'; u = 'N'; else, nm = 'Pressure'; u = 'psi'; end
        xlabel(ax, 'Time from the burn start (s)');
        ylabel(ax, sprintf('%s above the baseline (%s)', nm, u));
        if d == 0 || ~r.found
            title(ax, sprintf('%s: no strong peak found', nm));
            return;
        end
        k = r.tAll >= r.tStart - 2 & r.tAll <= r.tEnd + 2;
        x = r.tAll(k) - tRef; y = r.yAll(k);
        yl = [min(0, min(y)), max(y) * 1.1];
        patch(ax, [r.tStart r.tEnd r.tEnd r.tStart] - tRef, yl([1 1 2 2]), [0.85 0.92 1], 'EdgeColor', 'none');
        plot(ax, x, y, '-', 'Color', pal(d, :), 'LineWidth', 1.3);
        plot(ax, [x(1) x(end)], r.top5 * [1 1], '--', 'Color', [0.8 0 0], 'LineWidth', 1);
        plot(ax, [r.tStart r.tEnd] - tRef, r.avg * [1 1], '--', 'Color', [0 0.5 0], 'LineWidth', 1);
        legend(ax, {'burn (5 % to 5 %)', dname(d), 'top 5 % avg', 'average'}, 'Location', 'northeast');
        ylim(ax, yl);
        xlim(ax, [x(1), max(x(end), x(1) + 1e-3)]);
        if sn == 2
            title(ax, sprintf('Thrust: peak %.0f N, avg %.0f N, impulse %.0f N s, burn %.2f s', r.top5, r.avg, r.integral, r.burnS));
        else
            title(ax, sprintf('Pressure: peak %.0f psi, avg %.0f psi', r.top5, r.avg));
        end
    end

    function savePerf()
        if isempty(perfRes)
            perfLbl.Text = 'Nothing to save: push Analyze again first.';
            return;
        end
        base = fullfile(outDir, ['motor_performance_', char(datetime('now', 'Format', 'yyyy-MM-dd_HH-mm-ss'))]);
        fid = fopen([base, '.txt'], 'w');
        if fid == -1
            perfLbl.Text = ['Cannot write ', base, '.txt'];
            return;
        end
        fprintf(fid, '%s\n', perfTxt.Value{:});
        fclose(fid);
        fid = fopen([base, '_curve.csv'], 'w');
        fprintf(fid, 'series,time_s,time_from_burn_start_s,value,unit\n');
        rs = {perfRes.rF, perfRes.rP}; nm = {'thrust', 'pressure'}; un = {'N', 'psi'};
        for i = 1:2
            r = rs{i};
            if ~r.found, continue; end
            k = r.tAll >= r.tStart - 2 & r.tAll <= r.tEnd + 2;
            fprintf(fid, [nm{i}, ',%.4f,%.4f,%.6g,', un{i}, '\n'], [r.tAll(k); r.tAll(k) - perfRes.tRef; r.yAll(k)]);
        end
        fclose(fid);
        try
            exportgraphics(perfAxT, [base, '_thrust.png']);
            exportgraphics(perfAxP, [base, '_pressure.png']);
        catch
        end
        perfLbl.Text = ['Saved: ', base, '.txt, _curve.csv, .png'];
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
    end

    function closeApp()
        disconnectAll();
        if ~isempty(perfFig) && isvalid(perfFig), delete(perfFig); end
        delete(fig);
    end

    if isdeployed
        waitfor(fig);
    end
end

% ========================================================================= burn analysis
% The strongest peak of y(t) and its burn. minAmp: the smallest peak (above the baseline) that counts.
%   Noise    sigma from the differences of neighbour samples (the burn does not change it much).
%   Peak     the maximum of a 5-point moving median (one wrong sample is not a peak).
%   Baseline the median of the 2 s that end 0.2 s before the burn start. It is removed from all values.
%   Burn     from where the curve passes 5 % of the peak going up, to where it goes below 5 % after the peak
%            (crossing times are interpolated).
%   top5     the mean of the highest 5 % of the burn samples. avg = integral / burn time. integral: trapezoids.
function r = burnStats(t, y, minAmp)
    r = struct('found', false, 'msg', '', 'strength', 0, 'base', 0, 'tStart', NaN, 'tEnd', NaN, 'burnS', NaN, ...
        'tPeak', NaN, 'tMax', NaN, 'maxVal', NaN, 'top5', NaN, 'avg', NaN, 'integral', NaN, 'gapMax', 0, ...
        'cutStart', false, 'cutEnd', false, 'tAll', [], 'yAll', []);
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

% ========================================================================= plot helpers
function t = fmtDur(s)
    if s < 120
        t = sprintf('%.0f s', s);
    elseif s < 7200
        t = sprintf('%.1f min', s / 60);
    else
        t = sprintf('%.1f h', s / 3600);
    end
end

% At most maxPts points: each bucket of samples gives its minimum and its maximum (peaks stay visible). A bucket with a
% gap (NaN) gives a gap.
function [xp, yp] = minMaxDecimate(x, y, maxPts)
    m = numel(x);
    if m <= maxPts
        xp = x; yp = y;
        return;
    end
    step = ceil(m / (maxPts / 2));
    nb = ceil(m / step);
    pad = nb * step - m;                                   % repeat the last sample: the last bucket is full
    yb = reshape([y, repmat(y(end), 1, pad)], step, nb);
    xb = reshape([x, repmat(x(end), 1, pad)], step, nb);
    lo = min(yb, [], 1);
    hi = max(yb, [], 1);
    gap = any(isnan(yb), 1);
    lo(gap) = NaN; hi(gap) = NaN;
    xp = reshape([xb(1, :); xb(end, :)], 1, []);
    yp = reshape([lo; hi], 1, []);
end

% A NaN point after each step in t that is larger than thr (the line breaks there).
function [t2, v2] = insertGaps(t, v, thr)
    g = find(diff(t) > thr);
    if isempty(g)
        t2 = t; v2 = v;
        return;
    end
    k = zeros(1, numel(t));
    k(g + 1) = 1;
    pos = (1:numel(t)) + cumsum(k);
    t2 = zeros(1, numel(t) + numel(g));
    v2 = NaN(1, numel(t2));
    t2(pos) = t;
    v2(pos) = v;
    t2(pos(g) + 1) = t(g);
end

% ========================================================================= frames (server/telem_proto.h)
% A5 5B | ver | src_dev | type | len u16 | payload | CRC32 u32. The CRC covers ver .. payload. Little endian.

function f = tpFrame(type, pay)
    pay = reshape(uint8(pay), 1, []);
    body = [uint8(1), uint8(0), uint8(type), typecast(uint16(numel(pay)), 'uint8'), pay];
    f = [uint8(165), uint8(91), body, typecast(crc32(body), 'uint8')];
end

% The frame at buf(pos). st = 1: a frame (type, pay, src = the device ID of the board that made the data), next frame at nxt. st = 0: not complete, read more bytes.
% st = -1: no frame here, continue at nxt.
function [st, type, pay, nxt, src] = tpNext(buf, pos, checkCrc)
    type = 0;
    src = 0;
    pay = zeros(1, 0, 'uint8');
    nxt = pos;
    n = numel(buf) - pos + 1;
    if n < 2, st = 0; return; end
    if buf(pos) ~= 165 || buf(pos+1) ~= 91
        k = find(buf(pos+1:end) == 165, 1);
        if isempty(k), nxt = numel(buf) + 1; else, nxt = pos + k; end
        st = -1;
        return;
    end
    if n < 7, st = 0; return; end
    len = double(buf(pos+5)) + 256 * double(buf(pos+6));
    if buf(pos+2) ~= 1 || len > 16000, st = -1; nxt = pos + 1; return; end
    if n < 11 + len, st = 0; return; end
    if checkCrc && crc32(buf(pos+2 : pos+6+len)) ~= typecast(buf(pos+7+len : pos+10+len), 'uint32')
        st = -1; nxt = pos + 1;
        return;
    end
    type = double(buf(pos+4));
    src = double(buf(pos+3));
    pay = buf(pos+7 : pos+6+len);
    nxt = pos + 11 + len;
    st = 1;
end

% The 8-byte records of src/record.h: time u32 (as double), reading i16, seq u16. All are row vectors.
function [us, rd, sq] = decodeRecs(p)
    n = floor(numel(p) / 8);
    if n == 0
        us = zeros(1, 0); rd = zeros(1, 0, 'int16'); sq = zeros(1, 0, 'uint16');
        return;
    end
    m = reshape(p(1:8*n), 8, n);
    us = double(typecast(reshape(m(1:4, :), 1, []), 'uint32'));
    rd = typecast(reshape(m(5:6, :), 1, []), 'int16');
    sq = typecast(reshape(m(7:8, :), 1, []), 'uint16');
end

% Load cell count: i24 in bytes 4..6 (rec_lc_count in src/record.h).
function c = lcCount(rd, sq)
    c = mod(double(rd), 65536) + 65536 * double(bitand(sq, uint16(255)));
    c(c >= 2^23) = c(c >= 2^23) - 2^24;
end

% zlib CRC-32. Java when it is there (fast), else a table loop.
function v = crc32(b)
    persistent useJava tbl
    if isempty(useJava)
        useJava = false;
        try
            j = java.util.zip.CRC32();
            j.update(int8([1 2 3]));
            useJava = j.getValue() == 1438416925;
        catch
        end
        tbl = zeros(1, 256, 'uint32');
        for i = 0:255
            c = uint32(i);
            for k = 1:8
                if bitand(c, uint32(1))
                    c = bitxor(uint32(3988292384), bitshift(c, -1));      % 0xEDB88320
                else
                    c = bitshift(c, -1);
                end
            end
            tbl(i + 1) = c;
        end
    end
    b = reshape(uint8(b), 1, []);
    if useJava
        j = java.util.zip.CRC32();
        j.update(typecast(b, 'int8'));
        v = uint32(j.getValue());
    else
        c = uint32(4294967295);
        for i = 1:numel(b)
            c = bitxor(tbl(bitand(bitxor(c, uint32(b(i))), uint32(255)) + 1), bitshift(c, -8));
        end
        v = bitxor(c, uint32(4294967295));
    end
end
