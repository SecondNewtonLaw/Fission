//
// Fission.BinaryNinja plugin entry point.
//

#include <binaryninjaapi.h>

#include "Architecture.hpp"
#include "View.hpp"

BN_DECLARE_CORE_ABI_VERSION

extern "C" {
BINARYNINJAPLUGIN void CorePluginDependencies() {}

BINARYNINJAPLUGIN bool CorePluginInit() {
    fission::bn::RegisterArchitecture();
    fission::bn::RegisterView();
    return true;
}
}
