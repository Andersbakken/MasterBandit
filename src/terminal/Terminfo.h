#pragma once

#include <string>

// Emit a terminfo source-format entry derived from xtgettcapTable() (DCS.cpp).
// Single source of truth — the same table that answers XTGETTCAP at runtime
// produces the .terminfo file that gets compiled by tic. Callers pipe the
// result to `tic -x -o ~/.terminfo -` (the -x flag is mandatory; without it
// tic silently drops the mb-query-* and other extension caps).
std::string emitTerminfoSource();
