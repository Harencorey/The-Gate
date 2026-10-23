#pragma once
//==============================================================================
// Automatable parameter IDs. Never rename one after release: it breaks sessions.
//==============================================================================

namespace tg::pid
{
    inline constexpr const char* threshold  = "threshold";
    inline constexpr const char* hysteresis = "hysteresis";
    inline constexpr const char* attack     = "attack";
    inline constexpr const char* hold       = "hold";
    inline constexpr const char* release    = "release";
    inline constexpr const char* range      = "range";
    inline constexpr const char* lookahead  = "lookahead";
    inline constexpr const char* keySource  = "keySource";
    inline constexpr const char* keyHpf     = "keyHpf";
    inline constexpr const char* keyHighCut = "keyHighCut";
    inline constexpr const char* tracking   = "tracking";
    inline constexpr const char* listen     = "listen";

    // Phase 2
    inline constexpr const char* levelerOn   = "levelerOn";
    inline constexpr const char* levelAmount = "levelAmount";
    inline constexpr const char* levelTarget = "levelTarget";
    inline constexpr const char* levelMax    = "levelMax";
    inline constexpr const char* glowOn      = "glowOn";
    inline constexpr const char* glowType    = "glowType";
    inline constexpr const char* glowDrive   = "glowDrive";
    inline constexpr const char* glowCeiling = "glowCeiling";
}

namespace tg
{
    // Lookahead choices (index -> ms). Changing the lookahead changes the reported latency.
    inline constexpr float kLookaheadMs[] = { 0.0f, 2.0f, 5.0f, 10.0f, 20.0f };
    inline constexpr int   kLookaheadDefault = 2;   // 5 ms

    inline constexpr float kHpfOffHz      = 10.0f;     // the bottom of the HPF knob = Off
    inline constexpr float kHighCutOffHz  = 20000.0f;  // the top of the high-cut knob = Off
    inline constexpr float kTargetAutoDb  = -40.5f;    // the bottom of the leveler Target knob = Auto
}
