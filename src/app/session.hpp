#pragma once
// What every front end (vgrun, the Mac app, the iOS app) shares: checking the
// player's game folder against the manifest, the translated programs, the
// container folder, scripted input, and the headless runner.
#include <memory>
#include <string>

#include "host/machine.hpp"

namespace voy::app {

struct Options {
    host::Config config;
    int hash_every = 0;        // headless: print the frame hash every N frames
    std::string frame_dump;    // headless: write frames as PNG (prefix), with hash_every
};

// The translated programs built into this binary.
host::Programs built_programs();

// Checks the game folder against the built-in manifest (hashes of the
// programs and ROMs, presence and size of the data). Empty when it matches;
// otherwise what is wrong, for the player.
std::string verify_content(const std::string& folder);

// The default container folder for this platform.
std::string default_container();

// Runs the cabinet headless until --frames or quit; returns the exit status.
int run_headless(const Options& o);

}
