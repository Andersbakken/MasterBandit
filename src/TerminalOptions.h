#ifndef TERMINALOPTIONS_H
#define TERMINALOPTIONS_H

#include <string>

struct TerminalOptions {
    std::string shell;
    std::string user;
    int scrollbackLines = 4096;    // Tier 1 ring buffer capacity (power-of-2)
};

#endif /* TERMINALOPTIONS_H */
