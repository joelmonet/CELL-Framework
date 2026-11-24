// Original Cell Framework implementation; see LICENSE.
#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreAudioKit/AUViewController.h>
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
#else
#include <cstdio>
#include <cstdlib>
#endif

#include <cell/core.hpp>
#include <array>
#include <cmath>
#if !defined(CELL_AUV3_PROJECT)
#include "instrument_fixture.hpp"
#include "midi_fixture.hpp"
#endif

namespace {
#if defined(CELL_AUV3_PROJECT)
constexpr AudioComponentDescription effect{kAudioUnitType_Effect, CELL_TEST_AU_SUBTYPE, CELL_TEST_AU_MANUFACTURER, 0, 0};
constexpr auto success = "PASS\nThe effect restored state and rendered automation and bypass."
#if defined(CELL_AUV3_PROJECT_EDITOR)
    "\nCore Audio opened the Designer editor."
#endif
    ;
#else
using cell::auv3_example::midiEffect;
using cell::auv3_example::verifyMidiEffect;
using cell::auv3_example::instrument;
using cell::auv3_example::verifyInstrument;
constexpr AudioComponentDescription effect{0x61756678, 0x43763347, 0x43656c6c, 0, 0};
constexpr auto success = "PASS\nCell Gain restored state, rendered automation and bypass, and provided a custom editor.\n"
    "Cell MIDI Probe preserved note timing, followed scheduled transpose automation, and generated MIDI.\n"
    "Cell Instrument Probe rendered timed note-on, automation and note-off in stereo and mono.";
#endif
constexpr auto gain = cell::stableId("gain");
constexpr UInt32 frames = 128;
constexpr double sampleRate = 48000;
#if TARGET_OS_OSX
constexpr auto instantiationOptions = kAudioComponentInstantiation_LoadOutOfProcess;
#else
constexpr AudioComponentInstantiationOptions instantiationOptions{};
#endif

float inputValue(UInt32 channel) { return channel == 0 ? 0.25f : -0.5f; }

bool renderGain(AURenderBlock render, double& position, double db, unsigned blocks, OSStatus& status,
                UInt32 samplesToCheck = frames) {
    struct Buffers {
        UInt32 count;
        AudioBuffer channels[2];
    };
    static_assert(offsetof(Buffers, channels) == offsetof(AudioBufferList, mBuffers));
    std::array<std::array<float, frames>, 2> stereo{};
    Buffers buffers{2, {{1, sizeof(stereo[0]), stereo[0].data()},
                        {1, sizeof(stereo[1]), stereo[1].data()}}};
    AURenderPullInputBlock pull = ^AUAudioUnitStatus(AudioUnitRenderActionFlags*, const AudioTimeStamp*,
                                                    AUAudioFrameCount count, NSInteger bus,
                                                    AudioBufferList* data) {
        if (bus != 0 || data->mNumberBuffers != 2) return kAudioUnitErr_InvalidElement;
        for (UInt32 c = 0; c < 2; ++c) {
            auto& buffer = data->mBuffers[c];
            if (!buffer.mData || buffer.mDataByteSize < count * sizeof(float))
                return kAudioUnitErr_InvalidPropertyValue;
            std::fill_n(static_cast<float*>(buffer.mData), count, inputValue(c));
        }
        return noErr;
    };
    for (unsigned block = 0; block < blocks; ++block) {
        AudioUnitRenderActionFlags flags{};
        AudioTimeStamp stamp{};
        stamp.mSampleTime = position;
        stamp.mFlags = kAudioTimeStampSampleTimeValid;
        status = render(&flags, &stamp, frames, 0, reinterpret_cast<AudioBufferList*>(&buffers), pull);
        if (status != noErr || (flags & kAudioUnitRenderAction_OutputIsSilence)) return false;
        position += frames;
    }
    const auto level = std::pow(10.0, db / 20.0);
    for (UInt32 c = 0; c < 2; ++c)
        for (UInt32 frame = 0; frame < samplesToCheck; ++frame) {
            const auto sample = stereo[c][frame];
            if (!std::isfinite(sample) || std::abs(sample - level * inputValue(c)) >= 1e-6) return false;
        }
    return true;
}

NSString* verifyRecalledUnit(AUAudioUnit* unit, NSDictionary<NSString*, id>* state) {
    NSError* error = nil;
    unit.fullState = state;
    AUParameter* parameter = [unit.parameterTree parameterWithAddress:gain];
    if (!parameter || parameter.value != -6) return @"Reopened state did not restore −6 dB";
    AVAudioFormat* format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:sampleRate channels:2];
    if (![unit.inputBusses[0] setFormat:format error:&error] ||
        ![unit.outputBusses[0] setFormat:format error:&error]) return error.localizedDescription ?: @"Bus format rejected";
    const auto render = unit.renderBlock;
    const auto schedule = unit.scheduleParameterBlock;
    if (!render || !schedule) return @"Missing render or parameter scheduling block";
    unit.maximumFramesToRender = frames;
    unit.renderingOffline = YES;
    if (![unit allocateRenderResourcesAndReturnError:&error])
        return error.localizedDescription ?: @"Render allocation failed";
    OSStatus status{};
    double position{};
    NSString* failure = nil;
    if (!renderGain(render, position, -6, 1, status))
        failure = [NSString stringWithFormat:@"Recalled render failed (%d)", static_cast<int>(status)];
    else {
        constexpr UInt32 editFrame = frames / 2;
        schedule(static_cast<AUEventSampleTime>(position) + editFrame, 0, gain, 3);
        if (!renderGain(render, position, -6, 1, status, editFrame))
            failure = [NSString stringWithFormat:@"Audio changed before the scheduled edit (%d)", static_cast<int>(status)];
        else if (!renderGain(render, position, 3, 10, status))
            failure = [NSString stringWithFormat:@"Scheduled render failed (%d)", static_cast<int>(status)];
    }
    if (!failure) {
        constexpr UInt32 startFrame = frames / 2;
        schedule(static_cast<AUEventSampleTime>(position) + startFrame, 2 * frames, gain, -12);
        if (!renderGain(render, position, 3, 1, status, startFrame))
            failure = [NSString stringWithFormat:@"Audio changed before the scheduled ramp (%d)", static_cast<int>(status)];
        else if (!renderGain(render, position, -12, 10, status))
            failure = [NSString stringWithFormat:@"Scheduled ramp failed (%d)", static_cast<int>(status)];
    }
    if (!failure) {
        unit.shouldBypassEffect = YES;
        if (!renderGain(render, position, 0, 1, status))
            failure = [NSString stringWithFormat:@"Bypass render failed (%d)", static_cast<int>(status)];
    }
    [unit deallocateRenderResources];
    return failure;
}

void discover(AudioComponentDescription component, NSString* name, void (^complete)(AUAudioUnit*, NSString*),
              unsigned discoveryAttempts = 20) {
    if (!AudioComponentFindNext(nullptr, &component)) {
        if (discoveryAttempts == 0) {
            complete(nil, [NSString stringWithFormat:@"Core Audio did not discover %@", name]);
        } else {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 4),
                dispatch_get_main_queue(), ^{ discover(component, name, complete, discoveryAttempts - 1); });
        }
        return;
    }
    [AUAudioUnit instantiateWithComponentDescription:component options:instantiationOptions
        completionHandler:^(AUAudioUnit* unit, NSError* error) {
            NSString* failure = unit ? nil : (error.localizedDescription ?: [NSString stringWithFormat:@"Could not load %@", name]);
            complete(unit, failure);
        }];
}

void runGainCheck(void (^complete)(NSString*)) {
    discover(effect, @"AUv3 effect", ^(AUAudioUnit* unit, NSString* failure) {
        if (!unit) { complete(failure); return; }
        AUParameter* parameter = [unit.parameterTree parameterWithAddress:gain];
        if (!parameter || parameter.minValue != -60 || parameter.maxValue != 12 ||
            parameter.unit != kAudioUnitParameterUnit_Decibels ||
            !(parameter.flags & kAudioUnitParameterFlag_CanRamp)) {
            complete(@"Gain parameter metadata differs");
            return;
        }
        parameter.value = -6;
        NSDictionary<NSString*, id>* state = unit.fullState;
        if (!state[@"Cell.EngineState"]) { complete(@"The effect did not save state"); return; }
        [AUAudioUnit instantiateWithComponentDescription:effect options:instantiationOptions
            completionHandler:^(AUAudioUnit* reopened, NSError* error) {
                if (!reopened) { complete(error.localizedDescription ?: @"Could not reopen the AUv3 effect"); return; }
                if (NSString* failure = verifyRecalledUnit(reopened, state)) { complete(failure); return; }
#if !defined(CELL_AUV3_PROJECT) || defined(CELL_AUV3_PROJECT_EDITOR)
                if (!reopened.providesUserInterface) { complete(@"The effect did not advertise an AUv3 editor"); return; }
                [reopened requestViewControllerWithCompletionHandler:^(AUViewControllerBase* editor) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        // Keep the audio unit alive until the view has loaded.
                        (void)reopened;
                        complete(editor.view ? nil : @"Core Audio could not open the bundled editor");
                    });
                }];
#else
                complete(nil);
#endif
            }];
    });
}

void runCheck(void (^complete)(NSString*)) {
#if defined(CELL_AUV3_PROJECT)
    runGainCheck(complete);
#else
    runGainCheck(^(NSString* gainFailure) {
        discover(midiEffect, @"Cell MIDI Probe AUv3", ^(AUAudioUnit* unit, NSString* discoveryFailure) {
            NSString* midiFailure = unit ? verifyMidiEffect(unit) : discoveryFailure;
            discover(instrument, @"Cell Instrument Probe AUv3", ^(AUAudioUnit* synth, NSString* discoveryError) {
                NSString* instrumentFailure = synth ? verifyInstrument(synth) : discoveryError;
                if (!gainFailure && !midiFailure && !instrumentFailure) { complete(nil); return; }
                complete([NSString stringWithFormat:@"Cell Gain: %@\nCell MIDI Probe: %@\nCell Instrument Probe: %@",
                    gainFailure ?: @"PASS", midiFailure ?: @"PASS", instrumentFailure ?: @"PASS"]);
            });
        });
    });
#endif
}
} // namespace

#if TARGET_OS_IOS
@interface CellAUv3Scene : UIResponder<UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation CellAUv3Scene
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session
      options:(UISceneConnectionOptions*)options {
    (void)session;
    (void)options;
    if (![scene isKindOfClass:[UIWindowScene class]]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    UIViewController* view = [[UIViewController alloc] init];
    view.view.backgroundColor = UIColor.systemBackgroundColor;
    UILabel* label = [[UILabel alloc] init];
    label.text = @"Checking Cell AUv3 extensions…";
    label.textAlignment = NSTextAlignmentCenter;
    label.numberOfLines = 0;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [view.view addSubview:label];
    [NSLayoutConstraint activateConstraints:@[
        [label.centerYAnchor constraintEqualToAnchor:view.view.centerYAnchor],
        [label.leadingAnchor constraintEqualToAnchor:view.view.layoutMarginsGuide.leadingAnchor],
        [label.trailingAnchor constraintEqualToAnchor:view.view.layoutMarginsGuide.trailingAnchor],
    ]];
    self.window.rootViewController = view;
    [self.window makeKeyAndVisible];
    runCheck(^(NSString* failure) {
        NSString* result = failure ? [@"FAIL\n" stringByAppendingString:failure] :
            [NSString stringWithUTF8String:success];
        NSURL* documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
            inDomains:NSUserDomainMask].firstObject;
        [result writeToURL:[documents URLByAppendingPathComponent:@"AUv3-result.txt"]
            atomically:YES encoding:NSUTF8StringEncoding error:nil];
        dispatch_async(dispatch_get_main_queue(), ^{ label.text = result; });
    });
}
@end

@interface CellAUv3App : UIResponder<UIApplicationDelegate>
@end
@implementation CellAUv3App
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([CellAUv3App class]));
    }
}
#else
int main() {
    @autoreleasepool {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC),
            dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                std::fputs("FAIL\nTimed out waiting for the AUv3 checks.\n", stderr);
                std::exit(EXIT_FAILURE);
            });
        runCheck(^(NSString* failure) {
            if (failure) {
                std::fprintf(stderr, "FAIL\n%s\n", failure.UTF8String);
                std::exit(EXIT_FAILURE);
            }
            std::puts(success);
            std::exit(EXIT_SUCCESS);
        });
    }
    dispatch_main();
}
#endif
