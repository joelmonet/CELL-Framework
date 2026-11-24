ardour {
    ["type"] = "dsp",
    name = "Cell LV2 Stereo Fixture",
    description = "Independent stereo levels for the Cell LV2 host render test",
}

function dsp_ioconfig()
    return {{ audio_in = 2, audio_out = 2 }}
end

function dsp_run(ins, outs, n_samples)
    local left, right = outs[1]:array(), outs[2]:array()
    for i = 1, n_samples do
        left[i], right[i] = 0.25, -0.125
    end
end
