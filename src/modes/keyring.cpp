#include "keyring.hpp"

// C entry point from keyring-prompter code
extern "C" {
    int noctalia_keyring_main(int argc, char* argv[]);
}

namespace modes {

int runKeyring(int argc, char* argv[]) {
    return noctalia_keyring_main(argc, argv);
}

} // namespace modes
