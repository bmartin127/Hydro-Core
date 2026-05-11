#pragma once

// UE::IoStore::OnDemand::Mount discovery and IOnDemandIoStore singleton scan (UE 5.4+).

#include <cstdint>

namespace Hydro::Engine {
    extern void*    s_ioStoreOnDemandMount;
    extern uint8_t* s_ioStoreOnDemandSingletonGlobal;
    bool findIoStoreOnDemandMount();
    bool findIoStoreOnDemandVtable();
}
