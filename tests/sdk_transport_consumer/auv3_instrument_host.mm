// SPDX-License-Identifier: Apache-2.0
#include "instrument_fixture.hpp"
#include <iostream>

@interface CellAUv3Effect : AUAudioUnit
@end

int main() {
    @autoreleasepool {
        using namespace cell::auv3_example;
        [AUAudioUnit registerSubclass:CellAUv3Effect.class asComponentDescription:instrument
            name:@"Cell: Instrument Contract" version:1];
        NSError* error = nil;
        AUAudioUnit* unit = [[CellAUv3Effect alloc] initWithComponentDescription:instrument options:0 error:&error];
        NSString* failure = unit ? verifyInstrument(unit) :
            (error.localizedDescription ?: @"Could not create the instrument");
        if (failure) {
            std::cerr << failure.UTF8String << '\n';
            return 1;
        }
        std::cout << "AUv3 instrument note timing, parameter scheduling and bypass passed.\n";
    }
}
