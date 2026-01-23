#pragma once

namespace modes {

// Run keyring prompter mode (GCR system prompter)
// This uses GLib main loop, not Qt
int runKeyring(int argc, char* argv[]);

} // namespace modes
