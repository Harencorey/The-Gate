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

void TheGateLook::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos, float start, float end, Slider&)
{
    const auto bounds = Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
    const float r = jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto c = bounds.getCentre();
    const float angle = start + pos * (end - start);

    Path track;
    track.addCentredArc (c.x, c.y, r - 2, r - 2, 0.0f, start, end, true);
    g.setColour (colours::edge);
    g.strokePath (track, PathStrokeType (3.0f, PathStrokeType::curved, PathStrokeType::rounded));

    Path value;
    value.addCentredArc (c.x, c.y, r - 2, r - 2, 0.0f, start, angle, true);
    g.setColour (colours::accent);
    g.strokePath (value, PathStrokeType (3.0f, PathStrokeType::curved, PathStrokeType::rounded));

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
TheGateEditor::TheGateEditor (TheGateProcessor& p)
    : AudioProcessorEditor (&p), proc (p), display (p)
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
    makeKnob (keyHpf,     tg::pid::keyHpf,     "KEY HPF",    "High-pass on the key only (24 dB/oct): keeps kick and bass bleed from opening the gate. Bottom = Off.");
    makeKnob (keyHighCut, tg::pid::keyHighCut, "HIGH CUT",   "Low-pass on the key only (24 dB/oct): keeps cymbals from opening the gate. Top = Off.");
    makeKnob (tracking,   tg::pid::tracking,   "TRACKING",   "The high cut follows each hit: open on the strike to catch the attack, then closing as the hit decays, so cymbal bleed in the tail can't hold the gate open. 100 % = one octave per 12 dB of decay.");
    makeChoice (lookahead, tg::pid::lookahead, "LOOKAHEAD", "How far ahead the gate looks. More = cleaner openings, more latency (reported to the host). Use Off or 2 ms for live playing.");
    makeChoice (keySource, tg::pid::keySource, "KEY",       "What the gate listens to: its own input, or the sidechain input.");

    makeKnob (levelAmount, tg::pid::levelAmount, "AMOUNT", "How much of the gap to the target each hit closes. 100 % = every hit lands on the target; 50 % = halfway.");
    makeKnob (levelTarget, tg::pid::levelTarget, "TARGET", "The peak level hits are evened towards. Bottom = Auto: the running average of the last few hits, so the overall level stays put and only the hit-to-hit differences shrink.");
    makeKnob (levelMax,    tg::pid::levelMax,    "MAX",    "The most any single hit is turned up or down.");
    makeKnob (glowDrive,   tg::pid::glowDrive,   "DRIVE",  "Gain into Micro Glow. Below the ceiling it's just level; past it, peaks saturate.");
    makeKnob (glowCeiling, tg::pid::glowCeiling, "CEILING","The level peaks can't go past. Everything well below it is untouched.");
    makeChoice (glowType, tg::pid::glowType, "TYPE", "TUBE: Glow's tube output stage, soft with a little 2nd harmonic. DIODE: Glow's diode, softer and warmer (more 2nd). CLIP: near-hard and symmetric, odd harmonics only: shaves peaks flat.");

    auto setupOn = [this] (TextButton& b, const char* id, std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment>& att, const String& tip)
    {
        b.setClickingTogglesState (true);
        b.setTooltip (tip);
        addAndMakeVisible (b);
        att = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, id, b);
    };
    setupOn (levelerOn, tg::pid::levelerOn, levelerOnAtt, "Leveler on/off. Evens out soft and loud hits: each hit's peak is measured during the lookahead and its gain set before it plays, so transients are never ramped. Works best with 5 ms lookahead or more.");
    setupOn (glowOn,    tg::pid::glowOn,    glowOnAtt,    "Micro Glow on/off: a saturator/clipper from Glow at the output, after the gate and leveler, oversampled to ~384 kHz and linear-phase. Switching it never changes the latency.");

    listen.setClickingTogglesState (true);
    listen.setTooltip ("Hear the key exactly as the detector hears it (after the key HPF and high cut).");
    addAndMakeVisible (listen);
    listenAtt = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, tg::pid::listen, listen);

    setSize (820, 632);
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
    for (auto* k : { &glowDrive, &glowCeiling }) { k->slider.setAlpha (glw); k->label.setAlpha (glw); }
    glowType.box.setAlpha (glw); glowType.label.setAlpha (glw);
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
                                std::pair { levelArea, "LEVELER" }, std::pair { glowArea, "MICRO GLOW" } })
    {
        g.setColour (colours::panel);
        g.fillRoundedRectangle (area.toFloat(), 6.0f);
        g.setColour (colours::dim);
        g.setFont (FontOptions (10.0f, Font::bold));
        g.drawText (title, area.withHeight (18).reduced (10, 0), Justification::centredLeft);
    }

    const bool wantsSc = proc.apvts.getRawParameterValue (tg::pid::keySource)->load() > 0.5f;
    if (wantsSc && ! proc.sidechainConnected())
    {
        g.setColour (colours::accent);
        g.setFont (FontOptions (11.0f));
        g.drawText ("No sidechain input: route one to the plug-in's sidechain in your DAW", getLocalBounds().removeFromTop (40).reduced (160, 0), Justification::centred);
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
    detectArea = row.removeFromLeft (230);
    row.removeFromLeft (8);
    envArea = row.removeFromLeft (310);
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
        auto cols = columns (a.removeFromTop (120), 4);
        placeKnob (attack, cols[0]); placeKnob (hold, cols[1]); placeKnob (release, cols[2]); placeKnob (range, cols[3]);
    }
    {
        auto a = keyArea.reduced (8).withTrimmedTop (18);
        auto cols = columns (a.removeFromTop (120), 3);
        placeKnob (keyHpf, cols[0]); placeKnob (keyHighCut, cols[1]); placeKnob (tracking, cols[2]);
        auto bottom = a.reduced (4, 6);
        auto left = bottom.removeFromLeft (bottom.getWidth() / 2);
        keySource.label.setBounds (left.removeFromTop (14));
        keySource.box.setBounds (left.removeFromTop (24).reduced (10, 0));
        bottom.removeFromTop (14);
        listen.setBounds (bottom.removeFromTop (24).reduced (10, 0));
    }

    levelArea = row2.removeFromLeft (row2.getWidth() / 2 - 4);
    row2.removeFromLeft (8);
    glowArea = row2;
    levelerOn.setBounds (levelArea.getRight() - 54, levelArea.getY() + 6, 44, 20);
    glowOn.setBounds (glowArea.getRight() - 54, glowArea.getY() + 6, 44, 20);
    {
        auto a = levelArea.reduced (8).withTrimmedTop (22);
        auto cols = columns (a.removeFromTop (120), 3);
        placeKnob (levelAmount, cols[0]); placeKnob (levelTarget, cols[1]); placeKnob (levelMax, cols[2]);
    }
    {
        auto a = glowArea.reduced (8).withTrimmedTop (22);
        auto cols = columns (a.removeFromTop (120), 3);
        auto t = cols[0].reduced (4, 0);
        t.removeFromTop (34);
        glowType.label.setBounds (t.removeFromTop (16));
        glowType.box.setBounds (t.removeFromTop (26));
        placeKnob (glowDrive, cols[1]); placeKnob (glowCeiling, cols[2]);
    }
}
