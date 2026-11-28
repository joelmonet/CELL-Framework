-- Original Cell Framework implementation; see LICENSE.
local folder = assert(os.getenv("CELL_REFERENCE_RUN"))
local project = assert(os.getenv("CELL_REFERENCE_PLUGIN"))
local instrument = os.getenv("CELL_REFERENCE_INSTRUMENT") == "1"

local function report(message)
    local file = assert(io.open(folder .. "/host-result.tmp", "w"))
    file:write(message .. "\n")
    file:close()
    assert(os.rename(folder .. "/host-result.tmp", folder .. "/host-result.txt"))
end

reaper.InsertTrackAtIndex(0, false)
local track = reaper.GetTrack(0, 0)
reaper.GetSetMediaTrackInfo_String(track, "P_NAME", project, true)
local prefix = instrument and "CLAPi: " or "CLAP: "
local fx = reaper.TrackFX_AddByName(track, prefix .. project .. " (Cell Project)", false, -1)
if fx < 0 then
    report("FAIL REAPER could not load the staged CLAP plug-in " .. project)
    return
end
if instrument then
    local item = reaper.CreateNewMIDIItemInProj(track, 0, 2, false)
    local take = item and reaper.GetActiveTake(item)
    if take then
        local finish = reaper.MIDI_GetPPQPosFromProjTime(take, 1.5)
        for _, note in ipairs({60, 64, 67}) do
            reaper.MIDI_InsertNote(take, false, false, 0, finish, 0, note, 90, true)
        end
        reaper.MIDI_Sort(take)
        reaper.UpdateArrange()
    end
end
reaper.TrackFX_Show(track, fx, 3)

local deadline = reaper.time_precise() + 20
local function poll()
    if reaper.TrackFX_GetFloatingWindow(track, fx) then
        local _, name = reaper.TrackFX_GetFXName(track, fx, "")
        report("PASS REAPER " .. reaper.GetAppVersion() .. " opened " .. name)
    elseif reaper.time_precise() >= deadline then
        report("FAIL REAPER loaded " .. project .. " but its editor did not open")
    else
        reaper.defer(poll)
    end
end
poll()
