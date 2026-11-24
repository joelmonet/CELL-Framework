// SPDX-License-Identifier: Apache-2.0
#include "allocation_watch.hpp"
#include <cell/ksp.hpp>
#include <iostream>
#include <vector>
using namespace cell;
namespace {
void check(bool value, const char *reason) {
  if (!value)
    throw std::runtime_error(reason);
}
struct Capture : ksp::Output {
  std::array<ksp::VoiceCommand, 4096> commands{};
  unsigned count{};
  std::uint64_t base{};
  unsigned capacity = 4096;
  bool push(const ksp::VoiceCommand &value) noexcept override {
    if (count == capacity)
      return false;
    commands[count] = value;
    commands[count++].frame += unsigned(base);
    return true;
  }
};
MidiEvent note(unsigned pitch, unsigned frame = 0, bool down = true,
               unsigned channel = 1) {
  return {frame,
          {std::uint8_t((down ? 0x90 : 0x80) | (channel - 1)),
           std::uint8_t(pitch), 100},
          3};
}
bool process(ksp::Runtime &vm, std::span<const MidiEvent> events,
             unsigned frames, Capture &out) {
  cell_test::allocations = cell_test::deletions = 0;
  cell_test::audio = true;
  const bool result = vm.process(events, frames, out);
  cell_test::audio = false;
  check(!cell_test::allocations && !cell_test::deletions,
        "Script execution allocated or reclaimed on audio");
  out.base += frames;
  return result;
}
void articulation() {
  auto p = ksp::compile(R"(on init
declare $rr := 0
declare $released := 0
declare const $octave := 12
declare polyphonic $original
declare %roots[3] := (0, 2, 4)
end on
on note
$original := $EVENT_NOTE
disallow_group($ALL_GROUPS)
allow_group($rr mod 2)
inc($rr)
if ($EVENT_NOTE < 100)
change_note($EVENT_ID, $EVENT_NOTE + $octave + %roots[0])
else
ignore_event($EVENT_ID)
end if
change_vol($EVENT_ID, -3000, 0)
change_tune($EVENT_ID, 7000, 0)
end on
on release
$released := $released + $original
end on
)");
  ksp::Runtime vm;
  vm.prepare(p, 48000, 2);
  Capture out;
  check(process(vm, std::array{note(60, 3), note(64, 7)}, 64, out),
        "Articulation script failed");
  check(out.count == 2 && out.commands[0].kind == ksp::CommandKind::start &&
            out.commands[0].note == 72 && out.commands[0].frame == 3 &&
            out.commands[0].volumeMillidB == -3000 &&
            out.commands[0].tuneMillicents == 7000 &&
            out.commands[0].groups[0] == 1 && out.commands[1].groups[0] == 2,
        "Script group, pitch, gain, tune or sample offsets were lost");
  const auto first = out.commands[0].id, second = out.commands[1].id;
  check(process(vm, std::array{note(64, 1, false), note(60, 2, false)}, 64,
                out) &&
            out.commands[2].id == second && out.commands[3].id == first &&
            vm.variable("$released") == 124,
        "Polyphonic release state or transposed event identity was lost");
  check(vm.variable("$rr") == 2, "Global script variables did not persist");
  check(process(vm, std::array{note(110)}, 64, out) && out.count == 4,
        "Ignored input still started a voice");
}
void delayed() {
  auto p = ksp::compile(R"(on note
ignore_event($EVENT_ID)
wait(1000)
play_note($EVENT_NOTE + 12, $EVENT_VELOCITY, 50, 2000)
end on
)");
  ksp::Runtime vm;
  vm.prepare(p, 48000);
  Capture out;
  check(process(vm, std::array{note(60, 3)}, 64, out) && out.count == 1 &&
            out.commands[0].note == 72 && out.commands[0].frame == 51 &&
            out.commands[0].offsetMicroseconds == 50,
        "wait() blocked, used the wrong units or changed the generated sample "
        "offset");
  check(process(vm, {}, 64, out) && out.count == 1,
        "Timed note ended early at a block boundary");
  check(process(vm, {}, 64, out) && out.count == 2 &&
            out.commands[1].kind == ksp::CommandKind::release &&
            out.commands[1].frame == 147,
        "Timed note did not release at the correct sample");
  p = ksp::compile(R"(on note
play_note($EVENT_NOTE + 12, 80, 0, -1)
end on
)");
  vm.prepare(p, 48000);
  out = {};
  check(process(vm, std::array{note(48), note(48, 20, false)}, 64, out) &&
            out.count == 4 && out.commands[0].note == 60 &&
            out.commands[2].id == out.commands[0].id &&
            out.commands[3].id == out.commands[1].id,
        "Generated tied notes lost the originating key release");
  p = ksp::compile(R"(on note
wait(1000)
change_note($EVENT_ID, 80)
play_note($EVENT_NOTE, 90, 0, 1000)
end on
)");
  vm.prepare(p, 48000);
  out = {};
  check(process(vm, std::array{note(60)}, 64, out) &&
            out.commands[0].note == 60 && out.commands[1].note == 80 &&
            out.commands[1].frame == 48,
        "Post-wait change_note semantics incorrectly retuned the already "
        "sounding voice");
}
void controller() {
  auto p = ksp::compile(R"(on init
declare $id
end on
on controller
if ($CC_NUM = 64)
if (%CC[64] > 63)
$id := play_note(40, 99, 0, 0)
else
note_off($id)
end if
end if
end on
)");
  ksp::Runtime vm;
  vm.prepare(p, 48000);
  Capture out;
  const std::array<MidiEvent, 2> cc{
      {{4, {0xb6, 64, 127}, 3}, {40, {0xb6, 64, 0}, 3}}};
  check(process(vm, cc, 64, out) && out.count == 4 &&
            out.commands[0].channel == 7 && out.commands[0].oneShot &&
            out.commands[1].kind == ksp::CommandKind::midi &&
            out.commands[2].kind == ksp::CommandKind::release,
        "Controller script, MIDI forwarding or one-shot lifetime failed");
}
void bounds() {
  for (const auto text :
       {"on note\nset_engine_par(1,2,3,4,5)\nend on\n",
        "on note\ndeclare $x\nend on\n", "on note\nunknown_command()\nend on\n",
        "on init\ndeclare ui_knob $x(0,1,1)\nend on\n"}) {
    bool rejected = false;
    try {
      (void)ksp::compile(text);
    } catch (const ksp::CompileError &error) {
      rejected = error.line() > 0;
    }
    check(rejected, "Unsupported script behavior was silently accepted");
  }
  auto p = ksp::compile("on note\nwhile (1 = 1)\nend while\nend on\n");
  ksp::Runtime vm;
  ksp::Limits limits;
  limits.instructionsPerCallback = 128;
  vm.prepare(p, 48000, 1, limits);
  Capture out;
  check(!process(vm, std::array{note(60)}, 64, out) &&
            vm.fault() == ksp::Fault::instructionBudget &&
            out.commands[out.count - 1].kind == ksp::CommandKind::panic,
        "Unbounded script loop escaped the instruction budget");
  p = ksp::compile("on init\ndeclare %v[2]\nend on\non "
                   "note\nchange_note($EVENT_ID, %v[9])\nend on\n");
  vm.prepare(p, 48000);
  out = {};
  check(!process(vm, std::array{note(60)}, 64, out) &&
            vm.fault() == ksp::Fault::arrayBounds,
        "Script array escaped prepared storage");
  p = ksp::compile("on note\nwait(100000)\nend on\n");
  limits.callbacks = 1;
  vm.prepare(p, 48000, 1, limits);
  out = {};
  check(!process(vm, std::array{note(60), note(61)}, 64, out) &&
            vm.fault() == ksp::Fault::callbackCapacity,
        "Suspended callbacks exceeded their capacity");
  p = ksp::compile("on note\nchange_note($EVENT_ID, 70)\nend on\n");
  vm.prepare(p, 48000);
  out = {};
  out.capacity = 0;
  check(!process(vm, std::array{note(60)}, 64, out) &&
            vm.fault() == ksp::Fault::outputCapacity,
        "Voice output backpressure was ignored");
}
void partitions() {
  const auto p = ksp::compile("on "
                              "note\nignore_event($EVENT_ID)\nwait(1000)\nplay_"
                              "note($EVENT_NOTE,100,0,2000)\nend on\n");
  std::vector<ksp::VoiceCommand> reference;
  for (unsigned block : {17u, 64u, 128u}) {
    ksp::Runtime vm;
    vm.prepare(p, 48000);
    Capture out;
    const std::array<unsigned, 3> times{3, 53, 103};
    for (unsigned start = 0; start < 256; start += block) {
      const auto frames = std::min(block, 256 - start);
      std::array<MidiEvent, 3> events;
      unsigned n = 0;
      for (unsigned i = 0; i < times.size(); ++i)
        if (times[i] >= start && times[i] < start + frames)
          events[n++] = note(60 + i, times[i] - start);
      check(process(vm, {events.data(), n}, frames, out),
            "Partitioned script failed");
    }
    if (reference.empty())
      reference.assign(out.commands.begin(), out.commands.begin() + out.count);
    else {
      check(out.count == reference.size(),
            "Script callback count changed with the audio block size");
      for (unsigned i = 0; i < out.count; ++i)
        check(out.commands[i].kind == reference[i].kind &&
                  out.commands[i].note == reference[i].note &&
                  out.commands[i].frame == reference[i].frame,
              "Script scheduling changed with audio block size");
    }
  }
}
void userFunctionsAndSelect(){
 const auto program=ksp::compile(R"(on init
declare $result := 0
declare const $switch := 60
end on
function choose_articulation()
select ($EVENT_NOTE)
case 0 to 47
$result := 1
exit
case $switch
$result := 2
wait(1000)
change_vol($EVENT_ID, -6000, 0)
case 48 to 72
$result := 3
case 080000000H to 07FFFFFFFH
$result := 4
end select
$result := $result + 10
end function
function nested
call choose_articulation()
$result := $result + 100
end function
on note
call nested
$result := $result + 1000
end on
)");
 ksp::Runtime vm;vm.prepare(program,48000);Capture out;
 check(process(vm,std::array{note(40)},64,out)&&vm.variable("$result")==1101,"exit left the caller instead of the current function");
 check(process(vm,std::array{note(60)},64,out)&&vm.variable("$result")==1112,"select did not choose its first matching constant or preserve a waiting function");
 check(out.count==3&&out.commands[2].kind==ksp::CommandKind::volume&&out.commands[2].frame==112,"Function wait changed voice-event timing");
 check(process(vm,std::array{note(55)},64,out)&&vm.variable("$result")==1113,"select range did not match its inclusive bounds");
 check(process(vm,std::array{note(100)},64,out)&&vm.variable("$result")==1114,"Signed hexadecimal catch-all range failed");
 for(const auto script:{"on note\ncall absent\nend on\n","function bad\nunknown_command(1)\nend function\n","on note\nselect ($EVENT_NOTE)\ncase 4 to 2\nexit\nend select\nend on\n"}){bool rejected=false;try{(void)ksp::compile(script);}catch(const ksp::CompileError&){rejected=true;}check(rejected,"An invalid KSP function or case range was admitted");}
}

} // namespace
int main() {
  try {
    userFunctionsAndSelect();
    articulation();
    delayed();
    controller();
    bounds();
    partitions();
    std::cout
        << "PASS independent KSP bytecode, polyphonic release state, group "
           "round robins, event IDs, initial DSP parameters, non-blocking "
           "waits, generated/timed/tied notes, controller callbacks, "
           "block-invariant scheduling and allocation/overflow/budget guards\n";
  } catch (const std::exception &e) {
    cell_test::audio = false;
    std::cerr << e.what() << '\n';
    return 1;
  }
}
