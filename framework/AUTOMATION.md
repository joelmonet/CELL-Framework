# Parameter presentation and host events

`Processor::initializeParameters(ParameterBank&)` runs once after parameter and output attachment, during Engine construction. Use it to seed a host-facing bank from product defaults. Preparation must not reapply factory values to recalled controls.

`Processor::parameterChanged(ParamId,double) noexcept` receives accepted host events after the bank has been updated and before rendering the following sample segment. Values have been clamped. Invalid IDs, non-finite values and invalid frame ordering do not reach it. GUI gestures remain explicit Engine begin/value/end events. Never allocate, lock, destroy owned objects, load assets or prepare DSP in this callback.

`ParameterPresentationSource` lets a product expose actual titles, physical ranges, units, choices and visibility for a fixed host ID. Its revision changes when assignment metadata changes. The VST3 controller refreshes metadata and tells the host that titles/values have changed. Presentation queries run on the control thread; they may copy owning strings. They must not mutate audio state.

Products must keep IDs fixed and choose normalized storage independently from physical presentation. Restore module settings before seeding the host bank. Assignment must copy values from the receiving module to its host lanes. A stale lane value must never overwrite a receiving module during assignment. Structural changes require prepared-state publication outside audio; routine controls use already bound atomics. Changes arriving while a candidate is prepared must be transferred to the adopted candidate. Queues must retain the latest pending edit under backpressure.

Studio implements its product-specific 16 assignable module banks and 16 EQ band lanes separately from this SDK. The SDK imposes neither bank count nor module catalog. Inactive lanes are hidden. Ordinary module and extra-band controls apply on the sample boundary; structural modes are prepared on a worker. A module's current settings and group identity remain the saved-state authority.

`parameter_hooks_tests.cpp` checks construction order, seeded recall defaults, valid/clamped events at the correct sample boundary, GUI-event separation, repreparation and callback allocation/destruction. Product tests separately cover bank assignment, extra bands, graph handoff and host GUI gestures. Native Windows host qualification remains separate from Windows binaries run through Wine.

Keep the owning snapshot alive while iterating borrowed spans, nodes or string views. In C++20, passing a temporary owner through an accessor does not keep it alive for a later range loop. Tests must retain a named session/document before reading its nested parameter vectors. The Windows EQ menu test explicitly retains its saved-state owner.
