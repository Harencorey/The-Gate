#pragma once
//==============================================================================
// The Gate - the "hit" record.
//
// Every feature reads and writes hits: the live detector fills them in now; the
// classifier, pitch tracker, instance network and ARA analysis fill in the rest
// in later phases. Keeping one record from day one means nothing gets re-plumbed.
//==============================================================================

#include <cstdint>

namespace tg
{

struct Hit
{
    // Filled in by the gate core (Phase 1)
    uint64_t timeSamples   = 0;     // where the hit lands on the OUTPUT timeline (includes the full latency)
    uint64_t arrivalSample = 0;     // when the key first crossed the open threshold, on the input timeline
                                    // (the original arrival time - alignment must never overwrite this)
    float    peakDb        = -120.0f; // key peak over the first 10 ms of the hit (the hit's "velocity")
    float    audioPeakDb   = -120.0f; // audio peak measured before the hit reached the output (Phase 2)
    float    levelChangeDb = 0.0f;    // what the leveler did to this hit (Phase 2)

    // Filled in by later phases (defaults mean "not known yet")
    float    pitchHz       = 0.0f;  // Phase 4: fundamental at the strike
    float    confidence    = 1.0f;  // Phase 4: how sure the classifier is this is the source
    int      claimedBy     = -1;    // Phase 6: instance id that claimed the hit (-1 = this one / unclaimed)
    float    bleedExcessDb = 0.0f;  // Phase 6: energy above what the learned bleed map predicts
};

} // namespace tg
