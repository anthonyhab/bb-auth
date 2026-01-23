#pragma once

namespace modes {

// Run pinentry mode (GPG Assuan protocol)
// This is stdin/stdout based, no event loop needed
int runPinentry();

} // namespace modes
