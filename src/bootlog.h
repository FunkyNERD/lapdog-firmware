#pragma once
// Start-up timestamps: each step marks when it finished, and the USB console's
// "boot" command prints them. For finding a step that stalls - e.g. on a cold
// start from battery, where nothing can be watched until USB is plugged in.

#include <stdint.h>

namespace bootlog
{

void mark(const char *step); // step: a string literal
void print();

} // namespace bootlog
