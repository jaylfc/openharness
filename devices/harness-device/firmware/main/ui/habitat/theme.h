#pragma once

// One black surface, two neutral inks, one companion/action accent.
// Canvas is TRUE black: the panel is AMOLED, where a black pixel is simply off. It draws no power, has
// no backlight glow to lift it, and gives the inks their full contrast. (Upstream uses charcoal
// #181818 to match the desktop terminal pane; this fork trades that match for the glass.)
// All colors are RGB565-representable at full brightness. Conversion and the
// existing saved brightness setting remain at the UI boundary; no theme heap.
#define HT_THEME_CANVAS    0x000000u
#define HT_THEME_TEXT      0xefe7deu
#define HT_THEME_SECONDARY 0xada6adu
#ifdef DEVICE_HABITAT_ORANGE
#define HT_THEME_ACCENT    0xff6d00u
#else
#define HT_THEME_ACCENT    0xc6aaefu
#endif
#define HT_THEME_SELECTION 0x392c4au
#define HT_THEME_ERROR     0xe7a6adu
// A raised block behind a label — the tab pill on the Focus face.
//
// It is the one near-neutral dark colour besides the canvas, and near-neutral darks are a trap here:
// RGB565 has an extra green bit, so truncating one per channel tints it green. color() quantises the
// canvas to exact multiples of 8 for that reason and must do the same for this — see ui_habitat.c.
#define HT_THEME_CARD      0x282828u
// Desktop activityColor() / darkTerminalTheme ANSI status colors.
//
// The inbox status mark was the only coloured thing on the glass until the Focus skin, which reads a
// live status in HT_THEME_DONE and marks the engine in the accent. On the creature skins the rule is
// unchanged: pane name, message and navigation stay neutral.
// The recording waveform's green, and only that.
//
// It is a pure channel — brighter and harder than HT_THEME_DONE, which is the terminal's "finished"
// green and is what a status line uses. The old firmware kept the two apart for the same reason: the
// meter is the one thing on the glass that is supposed to shout, and a status line is not.
#define HT_THEME_VOICE     0x00ff2fu
#define HT_THEME_DONE      0x0dbc79u
#define HT_THEME_QUESTION  0xe5e510u
#define HT_THEME_FAILED    0xcd3131u
