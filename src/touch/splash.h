#pragma once
// The touch timer's boot splash: the LapDog mark barking, the wordmark, and a
// progress bar with the start-up step that's running (display::splash(),
// bootStep(), bootDone() in display.h). It paces itself so a fast boot still
// shows for a moment, fades the backlight in and out, and hands over to the
// main screen.

namespace splash
{

// Whether the splash is still up (the UI keeps its own screens off it till then).
bool active();

// Called (from LVGL's task, under its lock) to load the main screen at hand-over.
void onFinished(void (*load)());

} // namespace splash
