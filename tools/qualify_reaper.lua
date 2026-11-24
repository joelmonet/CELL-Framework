-- Run by qualify_reaper.py inside a fresh REAPER process.
local root = assert(os.getenv("CELL_REAPER_RUN"))
local format = assert(os.getenv("CELL_REAPER_FORMAT"))
local inspect = os.getenv("CELL_REAPER_INSPECT") == "1"
local rate = assert(tonumber(os.getenv("CELL_REAPER_RATE")))
local duration = assert(tonumber(os.getenv("CELL_REAPER_DURATION")))
local gain_db = assert(tonumber(os.getenv("CELL_REAPER_GAIN_DB")))
local report_phase = assert(os.getenv("CELL_REAPER_REPORT_PHASE"))
local steps = {}
for frame, gain in (os.getenv("CELL_REAPER_AUTOMATION") or ""):gmatch("(%d+):(-?%d+)") do
    steps[#steps + 1] = { time = tonumber(frame) / rate, gain = tonumber(gain) }
end
local names = {
    clap = "CLAP: Cell Gain Example (Cell SDK Example)",
    vst3 = "VST3: Cell Gain Example (Cell SDK Example)",
    jsfx = "JS: Cell Gain",
}

local function effect_name(track, fx)
    local ok, name = reaper.TrackFX_GetFXName(track, fx)
    assert(ok and name == names[format], "Loaded the wrong effect: " .. name)
    local _, parameter = reaper.TrackFX_GetParamName(track, fx, 0)
    assert(parameter:lower():find("gain", 1, true), "Unexpected parameter: " .. parameter)
    return name
end

local function current_gain(track, fx)
    if format == "jsfx" then
        return reaper.TrackFX_GetParam(track, fx, 0)
    end
    return reaper.TrackFX_GetParamNormalized(track, fx, 0) * 72 - 60
end

local function envelope_value(envelope, gain)
    -- REAPER stores CLAP and JSFX envelopes in plain units and VST3 in normalized units.
    local parameter_value = format == "vst3" and (gain + 60) / 72 or gain
    return reaper.ScaleToEnvelopeMode(reaper.GetEnvelopeScalingMode(envelope), parameter_value)
end

local function write_gain_envelope(track, fx)
    local envelope = assert(reaper.GetFXEnvelope(track, fx, 0, true), "Could not create gain envelope")
    reaper.DeleteEnvelopePointRange(envelope, -1, duration + 1)
    reaper.SetTrackAutomationMode(track, 1) -- Read mode.
    for _, step in ipairs(steps) do
        assert(reaper.InsertEnvelopePoint(envelope, step.time, envelope_value(envelope, step.gain),
                                        1, 0, false, true), "Could not insert a square gain step")
    end
    reaper.Envelope_SortPoints(envelope)
end

local function inspect_gain_envelope(track, fx)
    local envelope = assert(reaper.GetFXEnvelope(track, fx, 0, false), "The saved gain envelope is missing")
    assert(reaper.CountEnvelopePoints(envelope) == #steps, "The saved gain envelope has the wrong point count")
    for index, step in ipairs(steps) do
        local ok, time, value, shape = reaper.GetEnvelopePoint(envelope, index - 1)
        assert(ok and math.abs(time - step.time) < 0.5 / rate and shape == 1,
               ("The saved gain envelope differs at point %d"):format(index))
        assert(math.abs(value - envelope_value(envelope, step.gain)) < 0.00001,
               ("The saved gain envelope has the wrong gain at point %d"):format(index))
    end
end

local function inspect_project()
    local track = assert(reaper.GetTrack(0, 0), "The copied project has no track")
    assert(reaper.TrackFX_GetCount(track) == 1, "Expected one effect in the copied project")
    assert(reaper.TrackFX_GetEnabled(track, 0) and not reaper.TrackFX_GetOffline(track, 0),
           "The saved effect is bypassed or offline")
    local name = effect_name(track, 0)
    if #steps > 0 then
        inspect_gain_envelope(track, 0)
    else
        local observed = current_gain(track, 0)
        assert(math.abs(observed - gain_db) < 0.005,
               ("The saved gain reopened at %.5f dB, expected %.2f dB"):format(observed, gain_db))
    end
    return name
end

local function create_project()
    reaper.GetSetProjectInfo(0, "PROJECT_SRATE", rate, true)
    reaper.GetSetProjectInfo(0, "PROJECT_SRATE_USE", 1, true)
    reaper.InsertTrackAtIndex(0, false)
    local track = assert(reaper.GetTrack(0, 0))
    reaper.SetOnlyTrackSelected(track)
    assert(reaper.InsertMedia(root .. "/input.wav", 0) > 0, "Could not import the stimulus")
    local item = assert(reaper.GetTrackMediaItem(track, 0))
    reaper.SetMediaItemInfo_Value(item, "D_FADEINLEN", 0)
    reaper.SetMediaItemInfo_Value(item, "D_FADEOUTLEN", 0)
    reaper.SetMediaItemInfo_Value(item, "D_FADEINLEN_AUTO", 0)
    reaper.SetMediaItemInfo_Value(item, "D_FADEOUTLEN_AUTO", 0)

    local fx = reaper.TrackFX_AddByName(track, assert(names[format]), false, -1)
    assert(fx >= 0, "Could not instantiate " .. names[format])
    local name = effect_name(track, fx)
    if format == "jsfx" then
        assert(reaper.TrackFX_SetParam(track, fx, 0, gain_db), "Could not set JSFX gain in dB")
    else
        local normalized = (gain_db + 60) / 72 -- Reference gain ranges from -60 to +12 dB.
        assert(reaper.TrackFX_SetParamNormalized(track, fx, 0, normalized), "Could not set plug-in gain")
    end
    return track, fx, name
end

local function save_project(name)
    reaper.GetSetProjectInfo(0, "RENDER_SRATE", rate, true)
    reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
    reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true)
    reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0, true)
    reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", duration, true)
    reaper.GetSetProjectInfo(0, "RENDER_TAILFLAG", 0, true)
    reaper.GetSetProjectInfo(0, "RENDER_DITHER", 0, true)
    assert(reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "ZXZhdxgAAA==", true))
    assert(reaper.GetSetProjectInfo_String(0, "RENDER_FILE", root, true))
    assert(reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", format, true))
    -- Assign the filename, then perform a normal host save to refresh plug-in state.
    reaper.Main_SaveProjectEx(0, root .. "/" .. format .. ".rpp", 8)
    reaper.MarkProjectDirty(0)
    reaper.Main_SaveProject(0, false)
    return name
end

local function finish(ok, result)
    local report = assert(io.open(root .. "/" .. format .. "-" .. report_phase .. ".txt", "w"))
    report:write(ok and "PASS\n" or "FAIL\n", reaper.GetAppVersion(), "\n", result, "\n")
    report:close()
    if not ok and not inspect then
        reaper.OnStopButton()
        reaper.Main_SaveProjectEx(0, root .. "/" .. format .. "-failed.rpp", 8)
    end
    reaper.Main_OnCommand(40004, 0) -- File: Quit REAPER
end

if inspect then
    finish(xpcall(inspect_project, debug.traceback))
    return
end

local ok, track, fx, name = xpcall(create_project, debug.traceback)
if not ok then
    finish(false, track)
    return
end

-- Let REAPER deliver the host edit to the processor before saving its state.
reaper.OnPlayButton()
local deadline = reaper.time_precise() + 3
local function wait_for_gain()
    local observed = current_gain(track, fx)
    local position = reaper.GetPlayPosition()
    if position >= 0.1 and math.abs(observed - gain_db) < 0.005 then
        reaper.OnStopButton()
        reaper.defer(function()
            finish(xpcall(function()
                if #steps > 0 then write_gain_envelope(track, fx) end
                return save_project(name)
            end, debug.traceback))
        end)
    elseif reaper.time_precise() < deadline then
        reaper.defer(wait_for_gain)
    else
        finish(false, ("At %.3f s, observed %.5f dB, expected %.2f dB"):format(position, observed, gain_db))
    end
end
wait_for_gain()
