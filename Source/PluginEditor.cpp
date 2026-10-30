#include "PluginEditor.h"

using namespace juce;

namespace colours
{
    const Colour bg      { 0xff141518 };
    const Colour panel   { 0xff1c1e22 };
    const Colour edge    { 0xff2b2e34 };
    const Colour text    { 0xffd9dadd };
    const Colour dim     { 0xff7d828c };
    const Colour accent  { 0xffe8b04a };   // amber
    const Colour open    { 0xff3fbf8f };   // green: gate open
    const Colour key     { 0xff5aa9e6 };   // blue: key level
    const Colour level   { 0xffd98cf2 };   // violet: leveler
}

//==============================================================================
TheGateLook::TheGateLook()
{
    setColour (Slider::textBoxTextColourId, colours::text);
    setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    setColour (Label::textColourId, colours::dim);
    setColour (Label::outlineColourId, Colours::transparentBlack);
    setColour (TextEditor::outlineColourId, Colours::transparentBlack);
    setColour (TextEditor::focusedOutlineColourId, colours::accent);
    setColour (TextEditor::backgroundColourId, colours::bg);
    setColour (ComboBox::backgroundColourId, colours::panel);
    setColour (ComboBox::outlineColourId, colours::edge);
    setColour (ComboBox::textColourId, colours::text);
    setColour (ComboBox::arrowColourId, colours::dim);
    setColour (PopupMenu::backgroundColourId, colours::panel);
    setColour (PopupMenu::textColourId, colours::text);
    setColour (PopupMenu::highlightedBackgroundColourId, colours::edge);
    setColour (TextButton::textColourOffId, colours::dim);
    setColour (TextButton::textColourOnId, colours::bg);
    setColour (TooltipWindow::backgroundColourId, colours::panel);
    setColour (TooltipWindow::textColourId, colours::text);
    setColour (TooltipWindow::outlineColourId, colours::edge);
}

void TheGateLook::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos, float start, float end, Slider& s)
{
    const auto bounds = Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
    const float r = jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto c = bounds.getCentre();
    const float angle = start + pos * (end - start);

    if (s.getProperties().contains ("ring"))
    {
        // bipolar ring (the concentric pot's outer control), drawn from 12 o'clock
        const float rr = r - 2.0f, mid = 0.5f * (start + end);
        Path track;
        track.addCentredArc (c.x, c.y, rr, rr, 0.0f, start, end, true);
        g.setColour (colours::edge);
        g.strokePath (track, PathStrokeType (5.0f, PathStrokeType::curved, PathStrokeType::butt));
        if (std::abs (angle - mid) > 0.01f)
        {
            Path value;
            value.addCentredArc (c.x, c.y, rr, rr, 0.0f, jmin (mid, angle), jmax (mid, angle), true);
            g.setColour (colours::level);
            g.strokePath (value, PathStrokeType (5.0f, PathStrokeType::curved, PathStrokeType::butt));
        }
        g.setColour (colours::dim);
        g.fillEllipse (Rectangle<float> (3.0f, 3.0f).withCentre (c.getPointOnCircumference (rr + 6.0f, mid)));   // centre tick
        g.setColour (colours::text);
        g.fillEllipse (Rectangle<float> (7.0f, 7.0f).withCentre (c.getPointOnCircumference (rr, angle)));
        return;
    }

    Path track;
    track.addCentredArc (c.x, c.y, r - 2, r - 2, 0.0f, start, end, true);
    g.setColour (colours::edge);
    g.strokePath (track, PathStrokeType (3.0f, PathStrokeType::curved, PathStrokeType::rounded));

    Path value;
    value.addCentredArc (c.x, c.y, r - 2, r - 2, 0.0f, start, angle, true);
    g.setColour (colours::accent);
    g.strokePath (value, PathStrokeType (3.0f, PathStrokeType::curved, PathStrokeType::rounded));

    if (s.getProperties().contains ("mod"))   // live value incl. modulation, as a dot on the arc
    {
        const float mp = jlimit (0.0f, 1.0f, (float) s.getProperties()["mod"]);
        if (std::abs (mp - pos) > 0.004f)
        {
            g.setColour (colours::level);
            g.fillEllipse (Rectangle<float> (6.0f, 6.0f).withCentre (c.getPointOnCircumference (r - 2, start + mp * (end - start))));
        }
    }

    g.setColour (colours::panel.brighter (0.08f));
    g.fillEllipse (c.x - (r - 7), c.y - (r - 7), 2 * (r - 7), 2 * (r - 7));
    const Point<float> tip (c.x + (r - 10) * std::sin (angle), c.y - (r - 10) * std::cos (angle));
    g.setColour (colours::text);
    g.drawLine (Line<float> (c.getPointOnCircumference ((r - 7) * 0.35f, angle), tip), 2.0f);
}

void TheGateLook::drawButtonBackground (Graphics& g, Button& b, const Colour&, bool over, bool)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (b.getToggleState() ? colours::accent : (over ? colours::edge.brighter (0.1f) : colours::panel));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (colours::edge);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
}

//==============================================================================
void GateDisplay::pull()
{
    tg::MeterFrame f;
    bool any = false;
    while (proc.engine.meters.pop (f))
    {
        history[(size_t) head] = f;
        head = (head + 1) % kFrames;
        any = true;
    }
    if (any) repaint();
}

void GateDisplay::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (colours::panel);
    g.fillRoundedRectangle (r, 6.0f);
    auto plot = r.reduced (8.0f, 8.0f);

    const float floorDb = -80.0f;
    auto yFor = [&] (float db) { return plot.getY() + plot.getHeight() * (jlimit (floorDb, 0.0f, db) / floorDb); };

    // dB grid
    g.setFont (FontOptions (10.0f));
    for (float db : { -12.0f, -24.0f, -36.0f, -48.0f, -60.0f, -72.0f })
    {
        g.setColour (colours::edge);
        g.drawHorizontalLine ((int) yFor (db), plot.getX(), plot.getRight());
        g.setColour (colours::dim.withAlpha (0.6f));
        g.drawText (String ((int) db), Rectangle<float> (plot.getRight() - 28, yFor (db) - 12, 26, 11), Justification::right);
    }

    const int W = (int) plot.getWidth();
    if (W <= 0) return;
    const float framesPerPx = (float) kFrames / (float) W;

    Path inPath, keyPath, gainPath;
    for (int px = 0; px < W; ++px)
    {
        // oldest at the left
        const int f0 = (int) (px * framesPerPx), f1 = jmax (f0 + 1, (int) ((px + 1) * framesPerPx));
        float in = 0, key = -120, gain = 0, level = 0, hitDb = 0; bool open = false, hit = false;
        for (int f = f0; f < f1; ++f)
        {
            const auto& fr = history[(size_t) ((head + f) % kFrames)];
            in = jmax (in, fr.inPeak); key = jmax (key, fr.keyDb); gain = jmin (gain, fr.gainDb); open = open || fr.open;
            level = fr.levelDb;
            if (fr.hit) { hit = true; hitDb = fr.hitChangeDb; }
        }
        const float x = plot.getX() + (float) px;
        if (open)
        {
            g.setColour (colours::open.withAlpha (0.12f));
            g.fillRect (x, plot.getY(), 1.0f, plot.getHeight());
        }
        const float inDb = in > 1e-6f ? 20.0f * std::log10 (in) : -120.0f;
        // input level: mirrored "waveform" around the middle, scaled in dB
        const float hgt = plot.getHeight() * 0.5f * (1.0f - jlimit (floorDb, 0.0f, inDb) / floorDb);
        g.setColour (colours::dim.withAlpha (0.45f));
        g.fillRect (x, plot.getCentreY() - hgt, 1.0f, 2.0f * hgt);
        // what gets through (gate and leveler)
        const float outDb = inDb + gain + level;
        const float hOut = plot.getHeight() * 0.5f * (1.0f - jlimit (floorDb, 0.0f, outDb) / floorDb);
        g.setColour (colours::text.withAlpha (0.55f));
        g.fillRect (x, plot.getCentreY() - hOut, 1.0f, 2.0f * hOut);

        // leveler: a mark at each hit it moved (up = turned up, down = turned down)
        if (hit && std::abs (hitDb) >= 0.5f)
        {
            Path tri;
            const float ty = plot.getY() + 4.0f, sz = 4.0f;
            if (hitDb > 0) tri.addTriangle (x - sz, ty + sz * 1.6f, x + sz, ty + sz * 1.6f, x, ty);
            else           tri.addTriangle (x - sz, ty, x + sz, ty, x, ty + sz * 1.6f);
            g.setColour (colours::level);
            g.fillPath (tri);
        }
        if (px == 0) { keyPath.startNewSubPath (x, yFor (key)); gainPath.startNewSubPath (x, yFor (gain)); }
        else         { keyPath.lineTo (x, yFor (key));          gainPath.lineTo (x, yFor (gain)); }
    }
    g.setColour (colours::key);
    g.strokePath (keyPath, PathStrokeType (1.4f));
    g.setColour (colours::accent.withAlpha (0.8f));
    g.strokePath (gainPath, PathStrokeType (1.4f));

    // threshold lines (open, and close = open - hysteresis)
    const float thr = proc.apvts.getRawParameterValue (tg::pid::threshold)->load();
    const float hys = proc.apvts.getRawParameterValue (tg::pid::hysteresis)->load();
    g.setColour (colours::open);
    g.drawHorizontalLine ((int) yFor (thr), plot.getX(), plot.getRight());
    g.setColour (colours::open.withAlpha (0.45f));
    const float yc = yFor (thr - hys);
    for (float x = plot.getX(); x < plot.getRight(); x += 8.0f) g.drawLine (x, yc, x + 4.0f, yc, 1.0f);

    // legend, bottom-left
    const float ly = plot.getBottom() - 14;
    g.setColour (colours::bg.withAlpha (0.7f));
    g.fillRoundedRectangle (plot.getX() + 2, ly - 2, 290, 16, 3.0f);
    auto item = [&] (float x, Colour c, const char* t)
    {
        g.setColour (c); g.fillRect (x, ly + 5, 10.0f, 2.0f);
        g.setColour (colours::dim); g.drawText (t, Rectangle<float> (x + 14, ly, 60, 12), Justification::left);
    };
    item (plot.getX() + 8,   colours::key,    "KEY");
    item (plot.getX() + 52,  colours::accent, "GAIN");
    item (plot.getX() + 100, colours::open,   "THRESHOLD");
    g.setColour (colours::level);
    Path tri; tri.addTriangle (plot.getX() + 190, ly + 9, plot.getX() + 198, ly + 9, plot.getX() + 194, ly + 2);
    g.fillPath (tri);
    g.setColour (colours::dim);
    g.drawText ("LEVELED", Rectangle<float> (plot.getX() + 204, ly, 70, 12), Justification::left);
}

//==============================================================================
void SpectrumView::pull()
{
    tg::SpectrumSnapshot s;
    bool got = false;
    while (proc.engine.spectralDecay().snapshots.pop (s)) got = true;
    if (got) { snap = s; have = true; repaint(); }
}

void SpectrumView::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (colours::bg);
    g.fillRoundedRectangle (r, 4.0f);
    auto plot = r.reduced (4.0f, 6.0f);
    plot.removeFromBottom (10.0f);
    using S = tg::SpectrumSnapshot;
    auto xFor = [&] (float hz) { return plot.getX() + plot.getWidth() * std::log (hz / S::kMinHz) / std::log (S::kMaxHz / S::kMinHz); };
    g.setFont (FontOptions (9.0f));
    for (float hz : { 100.0f, 1000.0f, 10000.0f })
    {
        g.setColour (colours::edge);
        g.drawVerticalLine ((int) xFor (hz), plot.getY(), plot.getBottom());
        g.setColour (colours::dim.withAlpha (0.7f));
        g.drawText (hz >= 1000 ? String ((int) (hz / 1000)) + "k" : String ((int) hz), Rectangle<float> (xFor (hz) - 14, plot.getBottom(), 28, 10), Justification::centred);
    }
    if (! have || snap.hitsLearned < 2)
    {
        g.setColour (colours::dim);
        g.setFont (FontOptions (11.0f));
        g.drawText (have ? "learning... (" + String (snap.hitsLearned) + " hits)" : "learns from the first hits", plot, Justification::centred);
        return;
    }
    float top = -300.0f;
    for (int i = 0; i < S::kPoints; ++i) top = jmax (top, snap.sourceDb[i]);
    auto yFor = [&] (float db) { return plot.getY() + plot.getHeight() * jlimit (0.0f, 1.0f, (top - db) / 60.0f); };
    auto hzAt = [&] (int i) { return S::kMinHz * std::pow (S::kMaxHz / S::kMinHz, (i + 0.5f) / S::kPoints); };
    Path src, bl;
    src.startNewSubPath (plot.getX(), plot.getBottom());
    for (int i = 0; i < S::kPoints; ++i) src.lineTo (xFor (hzAt (i)), yFor (snap.sourceDb[i]));
    src.lineTo (plot.getRight(), plot.getBottom());
    src.closeSubPath();
    g.setColour (colours::level.withAlpha (0.35f));
    g.fillPath (src);
    if (snap.bleedLearned)
    {
        for (int i = 0; i < S::kPoints; ++i)
        {
            const Point<float> p (xFor (hzAt (i)), yFor (snap.bleedDb[i]));
            if (i == 0) bl.startNewSubPath (p); else bl.lineTo (p);
        }
        g.setColour (colours::dim);
        g.strokePath (bl, PathStrokeType (1.2f));
    }
    if (snap.bodyHz > 0.0f)
    {
        // the body band: what carries the sustain
        const float xb = xFor (jmax (S::kMinHz, snap.bodyHz));
        g.setColour (colours::accent.withAlpha (0.12f));
        g.fillRect (Rectangle<float> (plot.getX(), plot.getY(), xb - plot.getX(), plot.getHeight()));
        g.setColour (colours::accent);
        g.drawLine (xb, plot.getY(), xb, plot.getBottom(), 1.5f);
    }
    g.setColour (colours::dim);
    g.setFont (FontOptions (9.5f));
    g.drawText ("attack " + String (snap.transientMs, 0) + " ms", plot.reduced (3.0f, 1.0f), Justification::topRight);
}

//==============================================================================
ConcentricKnob::ConcentricKnob (AudioProcessorValueTreeState& apvts, const char* knobId, const char* ringId,
                                const String& kn, const String& rn, const String& knobTip, const String& ringTip)
    : knobName (kn), ringName (rn)
{
    for (Slider* s : { (Slider*) &ring, &knob })
    {
        s->setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
        s->setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        s->setPopupDisplayEnabled (false, false, nullptr);
        addAndMakeVisible (*s);
    }
    ring.getProperties().set ("ring", true);
    ring.setTooltip (ringTip);
    knob.setTooltip (knobTip);
    knobAtt = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (apvts, knobId, knob);
    ringAtt = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (apvts, ringId, ring);
    ring.setDoubleClickReturnValue (true, 0.0);
    knob.setDoubleClickReturnValue (true, 25.0);
    ring.onValueChange = [this] { repaint(); };
    knob.onValueChange = [this] { repaint(); };
}

bool ConcentricKnob::RingSlider::hitTest (int x, int y)
{
    const auto b = getLocalBounds().toFloat();
    const float d = b.getCentre().getDistanceFrom ({ (float) x, (float) y });
    const float r = jmin (b.getWidth(), b.getHeight()) * 0.5f;
    return d <= r && d >= r - 12.0f;   // just the ring
}

void ConcentricKnob::setModulated (float v)
{
    const auto prev = (float) knob.getProperties()["mod"];
    const float p = (float) knob.valueToProportionOfLength (v);
    if (std::abs (p - prev) > 0.002f || ! knob.getProperties().contains ("mod"))
    {
        knob.getProperties().set ("mod", p);
        knob.repaint();
    }
}

void ConcentricKnob::resized()
{
    auto b = getLocalBounds();
    b.removeFromTop (16);
    b.removeFromBottom (18);
    const int side = jmin (b.getWidth(), b.getHeight());
    dial = b.withSizeKeepingCentre (side, side);
    ring.setBounds (dial);
    knob.setBounds (dial.reduced (12));
}

void ConcentricKnob::paint (Graphics& g)
{
    auto b = getLocalBounds();
    g.setFont (FontOptions (11.0f, Font::bold));
    g.setColour (colours::dim);
    g.drawText (knobName, b.removeFromTop (16), Justification::centred);
    auto bottom = b.removeFromBottom (18);
    const String ringText = ring.getTextFromValue (ring.getValue());
    g.setFont (FontOptions (14.0f));
    g.setColour (colours::text);
    g.drawText (knob.getTextFromValue (knob.getValue()), bottom.removeFromLeft (bottom.getWidth() / 2).withTrimmedRight (6), Justification::centredRight);
    g.setFont (FontOptions (12.0f));
    g.setColour (std::abs (ring.getValue()) > 0.05 ? colours::level : colours::dim);
    g.drawText (ringName + " " + ringText, bottom.withTrimmedLeft (6), Justification::centredLeft);
}

//==============================================================================
TheGateEditor::TheGateEditor (TheGateProcessor& p)
    : AudioProcessorEditor (&p), proc (p), display (p),
      glowDrive (p.apvts, tg::pid::glowDrive, tg::pid::glowEnv, "DRIVE", "ENV",
                 "DRIVE (center knob): how hard Micro Glow is hit, on Glow's Drive scale. The output is loudness-matched, so this changes the color and density, not the level. Double-click: 25.",
                 "ENV (outer ring): the envelope follower. Hits that rise above the track's own average push Drive up (right of center) or pull it back (left), always returning to the knob setting. The violet dot on the knob shows where Drive is right now. Double-click: off."),
      spectrum (p)
{
    setLookAndFeel (&look);
    { tg::MeterFrame f; while (proc.engine.meters.pop (f)) {} }   // drop frames queued while the window was closed
    addAndMakeVisible (display);

    makeKnob (threshold,  tg::pid::threshold,  "THRESHOLD",  "Key level that opens the gate.");
    makeKnob (hysteresis, tg::pid::hysteresis, "HYSTERESIS", "How far below the threshold the key must fall before the gate closes (the dashed line). Stops chatter on decays.");
    makeKnob (attack,     tg::pid::attack,     "ATTACK",     "Opening time. With lookahead at least this long, the gate is fully open exactly when the transient arrives.");
    makeKnob (hold,       tg::pid::hold,       "HOLD",       "How long the gate stays open after the key falls below the close threshold.");
    makeKnob (release,    tg::pid::release,    "RELEASE",    "Time to close through the whole range.");
    makeKnob (range,      tg::pid::range,      "RANGE",      "How far the gate closes. All the way down = fully shut.");
    makeKnob (deBleed,    tg::pid::deBleed,    "DE-BLEED",   "Takes cymbal wash out of each hit's tail. Every stroke's attack passes untouched; after it, the highs (above ~2.5 kHz) may only die away as slowly as the drum's own body does - whatever rings on longer is cymbal, and it's turned down. The drum's low end is never touched, and it never opens or holds the gate. 0 % = off.");
    makeKnob (keyHpf,     tg::pid::keyHpf,     "KEY HPF",    "High-pass on the key only (24 dB/oct): keeps kick and bass bleed from opening the gate. Bottom = Off.");
    makeKnob (keyHighCut, tg::pid::keyHighCut, "HIGH CUT",   "Low-pass on the key only (24 dB/oct): keeps cymbals from opening the gate. Top = Off.");
    makeKnob (tracking,   tg::pid::tracking,   "TRACKING",   "The high cut follows each hit: open on the strike to catch the attack, then closing as the hit decays, so cymbal bleed in the tail can't hold the gate open. 100 % = one octave per 12 dB of decay.");
    makeChoice (lookahead, tg::pid::lookahead, "LOOKAHEAD", "How far ahead the gate looks. More = cleaner openings, more latency (reported to the host). Use Off or 2 ms for live playing.");
    makeChoice (keySource, tg::pid::keySource, "KEY",       "What the gate listens to: its own input, the sidechain input, or Source: its own input at the drum's learned fundamental, opening only on a stick attack - so cymbals and a drum ringing in sympathy can't open it.");

    makeKnob (levelAmount, tg::pid::levelAmount, "AMOUNT", "How much of the gap to the target each hit closes. 100 % = every hit lands on the target; 50 % = halfway.");
    makeKnob (levelTarget, tg::pid::levelTarget, "TARGET", "The peak level hits are evened towards. Bottom = Auto: the running average of the last few hits, so the overall level stays put and only the hit-to-hit differences shrink.");
    makeKnob (levelMax,    tg::pid::levelMax,    "MAX",    "The most any single hit is turned up or down.");
    addAndMakeVisible (glowDrive);
    addAndMakeVisible (spectrum);
    makeKnob (specTransient, tg::pid::specTransient, "TRANSIENT", "How long each hit's attack stays full range, untouched: learned from the drum's own stick attack. 0 % = as learned; -100 % = half as long; +100 % = twice as long.");
    makeKnob (specFocus,   tg::pid::specFocus,   "FOCUS",   "After the attack, the highs close from the top down as the hit decays (a low-pass that follows the drum): cymbal wash and hats go first, the drum's tone fades like it does on its own. 0 % = like the plain gate; 100 % = closes fast.");
    makeKnob (specSustain, tg::pid::specSustain, "SUSTAIN", "How much longer the drum's low end (its body band) rings past the gate: 0 % = closes with the gate; 50 % = rings 30 dB down; 100 % = 60 dB, never into the bleed. The highs never ring on.");
    spectrum.setTooltip ("What the spectral decay has learned from this track: the source (violet), the bleed when the gate is shut (grey line), the body band that carries the sustain (shaded) and the attack length.");
    makeChoice (glowType, tg::pid::glowType, "TYPE", "TUBE: Glow's tube output stage: the whole sound thickens, softer and rounder as Drive goes up. IRON: Glow's transformer: only the lows (below ~126 Hz) saturate and compress; mids and highs pass clean.");

    auto setupOn = [this] (TextButton& b, const char* id, std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment>& att, const String& tip)
    {
        b.setClickingTogglesState (true);
        b.setTooltip (tip);
        addAndMakeVisible (b);
        att = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, id, b);
    };
    setupOn (levelerOn, tg::pid::levelerOn, levelerOnAtt, "Leveler on/off. Evens out soft and loud hits: each hit's peak is measured during the lookahead and its gain set before it plays, so transients are never ramped. Works best with 5 ms lookahead or more.");
    setupOn (specOn,    tg::pid::specOn,    specOnAtt,    "Spectral decay on/off. Each hit's attack stays exactly the plain gate; then the highs close from the top down as the drum decays, and its low end can sustain. Zero latency, no phase shift.");
    setupOn (glowOn,    tg::pid::glowOn,    glowOnAtt,    "Micro Glow on/off: Glow's saturation at the output, after the gate and leveler. Oversampled to ~384 kHz, linear-phase, loudness-matched. Switching it never changes the latency.");

    // Learn: teach the gate this drum from a passage you pick
    makeKnob (strictness, tg::pid::strictness, "STRICTNESS", "How much like the learned drum an opening must sound before it's let through. 0 % = the learned check is off (the Source key alone). Higher = more bleed taken out, and more risk of losing a soft or odd hit. The check only ever takes openings away; it never opens the gate.");
    learnBtn.setClickingTogglesState (true);
    learnBtn.setTooltip ("Press, play (or loop) a passage with clean hits of this drum, press again. Every opening while it's lit is learned as the drum; stick attacks that don't open the gate are learned as bleed. Set the threshold so only real hits open it while you learn. Switches the Key to Source if it isn't already.");
    learnBtn.onClick = [this]
    {
        // Learn needs the Source key: pressing it on another key switches the key to Source first
        if (learnBtn.getToggleState() && (int) std::lround (proc.apvts.getRawParameterValue (tg::pid::keySource)->load()) != 2)
            if (auto* k = proc.apvts.getParameter (tg::pid::keySource))
            {
                k->beginChangeGesture(); k->setValueNotifyingHost (k->convertTo0to1 (2.0f)); k->endChangeGesture();
            }
        proc.learnArmed.store (learnBtn.getToggleState());
    };
    learnBtn.setToggleState (proc.learnArmed.load(), dontSendNotification);
    addAndMakeVisible (learnBtn);
    clearBtn.setTooltip ("Forget what was learned (back to the Source key alone).");
    clearBtn.onClick = [this] { proc.clearLearned(); };
    addAndMakeVisible (clearBtn);

    listen.setClickingTogglesState (true);
    listen.setTooltip ("Hear the key exactly as the detector hears it (after the key HPF and high cut).");
    addAndMakeVisible (listen);
    listenAtt = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, tg::pid::listen, listen);

    setSize (980, 632);
    startTimerHz (30);
}

TheGateEditor::~TheGateEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void TheGateEditor::makeKnob (Knob& k, const char* id, const String& name, const String& tip)
{
    k.slider.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (Slider::TextBoxBelow, false, 72, 16);
    k.slider.setTooltip (tip);
    k.slider.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    addAndMakeVisible (k.slider);
    k.label.setText (name, dontSendNotification);
    k.label.setJustificationType (Justification::centred);
    k.label.setFont (FontOptions (11.0f, Font::bold));
    addAndMakeVisible (k.label);
    k.att = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, k.slider);
}

void TheGateEditor::makeChoice (Choice& c, const char* id, const String& name, const String& tip)
{
    if (auto* p = dynamic_cast<AudioParameterChoice*> (proc.apvts.getParameter (id)))
        c.box.addItemList (p->choices, 1);
    c.box.setTooltip (tip);
    addAndMakeVisible (c.box);
    c.label.setText (name, dontSendNotification);
    c.label.setJustificationType (Justification::centred);
    c.label.setFont (FontOptions (11.0f, Font::bold));
    addAndMakeVisible (c.label);
    c.att = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, id, c.box);
}

void TheGateEditor::timerCallback()
{
    display.pull();
    // dim a section's controls while it's off
    const float lvl = levelerOn.getToggleState() ? 1.0f : 0.4f, glw = glowOn.getToggleState() ? 1.0f : 0.4f;
    for (auto* k : { &levelAmount, &levelTarget, &levelMax }) { k->slider.setAlpha (lvl); k->label.setAlpha (lvl); }
    glowDrive.setAlpha (glw);
    const float spc = 0.0f;   // parked
    for (auto* k : { &specTransient, &specFocus, &specSustain }) { k->slider.setAlpha (spc); k->label.setAlpha (spc); }
    spectrum.setAlpha (spc);
    repaint (sourceNote);
    repaint (learnNote);
    {
        const bool src = (int) std::lround (proc.apvts.getRawParameterValue (tg::pid::keySource)->load()) == 2;
        learnBtn.setEnabled (true);
        // in Source mode the gate listens at the drum's fundamental: the key HPF / high cut / tracking don't apply
        const float kf = src ? 0.35f : 1.0f;
        for (auto* k : { &keyHpf, &keyHighCut, &tracking }) { k->slider.setAlpha (kf); k->label.setAlpha (kf); }
        const bool have = proc.engine.uiProfileHits.load() > 0;
        clearBtn.setEnabled (have);
        const float a = src && have ? 1.0f : 0.4f;
        strictness.slider.setAlpha (a); strictness.label.setAlpha (a);
    }
    glowType.box.setAlpha (glw); glowType.label.setAlpha (glw);
    glowDrive.setModulated (proc.engine.glowModulatedDrive());
}

void TheGateEditor::paint (Graphics& g)
{
    g.fillAll (colours::bg);
    auto header = getLocalBounds().removeFromTop (40).reduced (16, 0);
    g.setColour (colours::text);
    g.setFont (FontOptions (20.0f, Font::bold));
    g.drawText ("THE GATE", header, Justification::centredLeft);
    g.setColour (colours::dim);
    g.setFont (FontOptions (11.0f));
    g.drawText ("NORKA DSP   v" + String (JucePlugin_VersionString), header, Justification::centredRight);

    for (auto [area, title] : { std::pair { detectArea, "DETECT" }, std::pair { envArea, "ENVELOPE" }, std::pair { keyArea, "KEY FILTERS" },
                                std::pair { levelArea, "LEVELER" }, std::pair { specArea, "LEARN" }, std::pair { glowArea, "MICRO GLOW" } })
    {
        g.setColour (colours::panel);
        g.fillRoundedRectangle (area.toFloat(), 6.0f);
        g.setColour (colours::dim);
        g.setFont (FontOptions (10.0f, Font::bold));
        g.drawText (title, area.withHeight (18).reduced (10, 0), Justification::centredLeft);
    }

    const int keyMode = (int) std::lround (proc.apvts.getRawParameterValue (tg::pid::keySource)->load());
    const bool wantsSc = keyMode == 1;
    if (wantsSc && ! proc.sidechainConnected())
    {
        g.setColour (colours::accent);
        g.setFont (FontOptions (11.0f));
        g.drawText ("No sidechain input: route one to the plug-in's sidechain in your DAW", getLocalBounds().removeFromTop (40).reduced (160, 0), Justification::centred);
    }
    if (keyMode == 2)
    {
        // the Source key's learned fundamental, under the Key menu
        const double f0 = proc.engine.sourceKeyHz();
        g.setColour (colours::level);
        g.setFont (FontOptions (11.0f));
        g.drawText (f0 > 0.0 ? "listening at " + String (roundToInt (f0)) + " Hz + stick attack (15 ms lookahead)" : "learning the drum... (stick attack only)",
                    sourceNote, Justification::centred);
    }
    // Learn status
    {
        auto& e = proc.engine;
        const int have = e.uiProfileHits.load();
        String line1, line2;
        Colour c = colours::text;
        if (keyMode != 2 && ! learnBtn.getToggleState())
        {
            line1 = "Learn uses Key = Source: pressing LEARN switches to it.";
            line2 = have > 0 ? "A profile from " + String (have) + " hits is kept for when you switch back." : String();
            c = colours::dim;
        }
        else if (learnBtn.getToggleState())
        {
            line1 = "Learning: " + String (e.uiLearnHits.load()) + " hits, " + String (e.uiLearnBleeds.load()) + " bleed events";
            line2 = "Play a passage with clean hits, then press LEARN again.";
            c = colours::accent;
        }
        else if (e.uiLearnFailed.load())
        {
            line1 = "Not enough to learn from: it needs 4+ hits that open";
            line2 = "the gate and 10+ stick attacks from other drums that don't.";
            c = colours::accent;
        }
        else if (have > 0)
        {
            line1 = "Learned from " + String (have) + " hits. Checking every opening.";
            line2 = "Openings taken back as bleed: " + String (e.uiVetoes.load());
            c = colours::level;
        }
        else
        {
            line1 = "Press LEARN, play a passage with clean hits of this";
            line2 = "drum, press LEARN again. Off until then.";
            c = colours::dim;
        }
        g.setColour (c);
        g.setFont (FontOptions (12.0f));
        auto ln = learnNote;
        g.drawText (line1, ln.removeFromTop (18), Justification::centredLeft);
        g.setColour (colours::dim);
        g.drawText (line2, ln.removeFromTop (18), Justification::centredLeft);
        g.setFont (FontOptions (10.0f));
        g.drawText ("Spectral decay is parked until Phase 6.", ln.removeFromBottom (14), Justification::centredLeft);
    }
}

void TheGateEditor::resized()
{
    auto r = getLocalBounds().reduced (12);
    r.removeFromTop (30);
    display.setBounds (r.removeFromTop (190));
    r.removeFromTop (10);

    auto row = r.removeFromTop (210);
    r.removeFromTop (8);
    auto row2 = r;
    detectArea = row.removeFromLeft (270);
    row.removeFromLeft (8);
    envArea = row.removeFromLeft (360);
    row.removeFromLeft (8);
    keyArea = row;

    auto placeKnob = [] (Knob& k, Rectangle<int> a)
    {
        k.label.setBounds (a.removeFromTop (16));
        k.slider.setBounds (a);
    };
    auto columns = [] (Rectangle<int> a, int n)
    {
        std::vector<Rectangle<int>> out;
        const int w = a.getWidth() / n;
        for (int i = 0; i < n; ++i) out.push_back (a.removeFromLeft (w));
        return out;
    };

    {
        auto a = detectArea.reduced (8).withTrimmedTop (18);
        auto cols = columns (a.removeFromTop (120), 2);
        placeKnob (threshold, cols[0]);
        placeKnob (hysteresis, cols[1]);
        auto bottom = a.reduced (4, 6);
        lookahead.label.setBounds (bottom.removeFromTop (14));
        lookahead.box.setBounds (bottom.removeFromTop (24).reduced (30, 0));
    }
    {
        auto a = envArea.reduced (8).withTrimmedTop (18);
        auto cols = columns (a.removeFromTop (120), 5);
        placeKnob (attack, cols[0]); placeKnob (hold, cols[1]); placeKnob (release, cols[2]); placeKnob (range, cols[3]); placeKnob (deBleed, cols[4]);
    }
    {
        auto a = keyArea.reduced (8).withTrimmedTop (18);
        auto cols = columns (a.removeFromTop (120), 3);
        placeKnob (keyHpf, cols[0]); placeKnob (keyHighCut, cols[1]); placeKnob (tracking, cols[2]);
        auto bottom = a.reduced (4, 6);
        auto left = bottom.removeFromLeft (bottom.getWidth() / 2);
        keySource.label.setBounds (left.removeFromTop (14));
        keySource.box.setBounds (left.removeFromTop (24).reduced (10, 0));
        sourceNote = left.removeFromTop (16).expanded (60, 0).translated (30, 2);
        bottom.removeFromTop (14);
        listen.setBounds (bottom.removeFromTop (24).reduced (10, 0));
    }

    levelArea = row2.removeFromLeft (262);
    row2.removeFromLeft (8);
    specArea = row2.removeFromLeft (406);
    row2.removeFromLeft (8);
    glowArea = row2;
    levelerOn.setBounds (levelArea.getRight() - 54, levelArea.getY() + 6, 44, 20);
    specOn.setBounds (specArea.getRight() - 54, specArea.getY() + 6, 44, 20);
    {
        auto a = specArea.reduced (8).withTrimmedTop (22);
        auto k = a.removeFromLeft (100);
        placeKnob (strictness, k.removeFromTop (120));
        a.removeFromLeft (10);
        auto btns = a.removeFromTop (30);
        learnBtn.setBounds (btns.removeFromLeft (120).reduced (0, 2));
        btns.removeFromLeft (10);
        clearBtn.setBounds (btns.removeFromLeft (80).reduced (0, 2));
        a.removeFromTop (8);
        learnNote = a;
    }
    {
        auto a = specArea.reduced (8).withTrimmedTop (22);
        auto knobs = a.removeFromLeft (234);
        auto cols = columns (knobs.removeFromTop (120), 3);
        placeKnob (specTransient, cols[0]); placeKnob (specFocus, cols[1]); placeKnob (specSustain, cols[2]);
        spectrum.setBounds (a.withTrimmedLeft (4).withTrimmedBottom (4));
    }
    for (Component* c : { (Component*) &specOn, (Component*) &spectrum, (Component*) &specTransient.slider, (Component*) &specTransient.label,
                          (Component*) &specFocus.slider, (Component*) &specFocus.label, (Component*) &specSustain.slider, (Component*) &specSustain.label })
        c->setVisible (false);   // parked
    glowOn.setBounds (glowArea.getRight() - 54, glowArea.getY() + 6, 44, 20);
    {
        auto a = levelArea.reduced (8).withTrimmedTop (22);
        auto cols = columns (a.removeFromTop (120), 3);
        placeKnob (levelAmount, cols[0]); placeKnob (levelTarget, cols[1]); placeKnob (levelMax, cols[2]);
    }
    {
        auto a = glowArea.reduced (8, 4).withTrimmedTop (14);
        auto t = a.removeFromLeft (a.getWidth() * 7 / 20).reduced (8, 0);
        t.removeFromTop ((t.getHeight() - 42) / 2);
        glowType.label.setBounds (t.removeFromTop (16));
        glowType.box.setBounds (t.removeFromTop (26));
        glowDrive.setBounds (a.withTrimmedBottom (2));
    }
}
