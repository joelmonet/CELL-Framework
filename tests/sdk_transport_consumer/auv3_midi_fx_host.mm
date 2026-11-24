// SPDX-License-Identifier: Apache-2.0
#include "midi_fixture.hpp"
#include <iostream>

@interface CellAUv3Effect : AUAudioUnit
@end

int main() {
    @autoreleasepool {
        using namespace cell::auv3_example;
        [AUAudioUnit registerSubclass:CellAUv3Effect.class asComponentDescription:midiEffect
            name:@"Cell: MIDI Effect Contract" version:1];
        NSError* error = nil;
        AUAudioUnit* unit = [[CellAUv3Effect alloc] initWithComponentDescription:midiEffect options:0 error:&error];
        NSString* failure = unit ? verifyMidiEffect(unit, true) :
            (error.localizedDescription ?: @"Could not create the MIDI effect");
        if (failure) {
            std::cerr << failure.UTF8String << '\n';
            return 1;
        }
        std::cout << "AUv3 MIDI processor scheduling, clock bus silence and zero-channel rendering passed.\n";
    }
}
