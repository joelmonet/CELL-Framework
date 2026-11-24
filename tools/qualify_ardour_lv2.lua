-- Create snapshots for the Ardour LV2 release check.
local session_dir, rate, frames, uri = arg[1], tonumber(arg[2]), tonumber(arg[3]), arg[4]
local session = assert(create_session(session_dir, "CellLV2", rate), "Could not create session")
local tracks = session:new_audio_track(2, 2, nil, 1, "Cell LV2 test",
    ARDOUR.PresentationInfo.max_order, ARDOUR.TrackMode.Normal, true)
local track = tracks:front()
assert(not track:isnil(), "Could not create stereo track")

local source = ARDOUR.LuaAPI.new_plugin(session, "Cell LV2 Stereo Fixture", ARDOUR.PluginType.Lua, "")
assert(not source:isnil(), "Ardour could not discover the stimulus script")
assert(track:add_processor_by_index(source, 0, nil, true) == 0, "Could not insert stimulus")
session:set_session_extents(Temporal.timepos_t(0), Temporal.timepos_t(frames))
assert(session:save_state("dry") == 0, "Could not save dry snapshot")

local effect = ARDOUR.LuaAPI.new_plugin(session, uri, ARDOUR.PluginType.LV2, "")
assert(not effect:isnil(), "Ardour could not discover LV2 effect " .. uri)
assert(track:add_processor_by_index(effect, 1, nil, true) == 0, "Could not insert Cell Gain")
for i = 5, #arg, 2 do
    local snapshot, gain = arg[i], tonumber(arg[i + 1])
    assert(ARDOUR.LuaAPI.set_processor_param(effect, 0, gain), "Could not set the gain")
    assert(session:save_state(snapshot) == 0, "Could not save snapshot " .. snapshot)
end
close_session()
