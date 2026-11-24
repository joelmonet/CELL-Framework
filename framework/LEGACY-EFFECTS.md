# LV2, LADSPA, DSSI and REAPER JSFX

Build the Linux reference gain effect with these CMake options:

```sh
cmake -S framework -B build-formats -DCELL_SDK_EXAMPLES=ON -DCELL_SDK_TESTS=ON \
  -DCELL_GAIN_LV2_SDK=/path/to/lv2/prefix \
  -DCELL_GAIN_LADSPA_INCLUDE_DIR=/path/to/include -DCELL_GAIN_DSSI=ON
cmake --build build-formats
ctest --test-dir build-formats -R '^cell_(lv2|ladspa|dssi)_host$' --output-on-failure
```

The LV2 prefix contains `include/lv2/core/lv2.h`. The LADSPA include path
contains `ladspa.h`. DSSI also needs `dssi.h` and the ALSA headers. Each
format can be enabled independently, with the include path shared by the
LADSPA and DSSI examples. The outputs are in the gain example build directory.
Keep the two Turtle files together with the binary when installing the LV2
bundle. Install the LADSPA and DSSI variants into their respective format
directories. They represent the same effect and use the same LADSPA identity.

Run the [Ardour LV2 qualification](VALIDATION.md#ardour-lv2-qualification)
to check bundle discovery, project recall, and rendering through a DAW.

`cell_add_lv2_plugin` takes a processor target, stable URI, SDK prefix and
Turtle description. The description must match the URI and public port order:
planar inputs, planar outputs, then physical-valued control inputs in descriptor
order. `cell_add_ladspa_plugin` takes a processor, include directory, stable
label and unique numeric ID. Passing `DSSI` adds the DSSI effect entry point.
Assign product-specific IDs and URI before distributing a plugin.

The adapters target fixed-layout audio effects with block-rate control inputs.
Their reference tests cover exported descriptors, instantiation, in-place audio,
long host buffers, gain changes, cleanup, and reload. The current scope is audio
effects. MIDI synths, native editors, atom ports, latency reporting and
format-specific state extensions require additional adapters.

Copy `examples/sdk_gain/Cell Gain.jsfx` to a folder under REAPER's resource
path `Effects` and rescan the effect list. It uses a stereo signal path,
a −60 to +12 dB slider, and a 10 ms sample-based gain ramp. It runs as an
independent JSFX script. The Linux validation used the ysfx runtime.
