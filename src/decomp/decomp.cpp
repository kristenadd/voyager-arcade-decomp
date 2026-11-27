#include "decomp/decomp.hpp"

#include <cstdlib>
#include <cstring>

namespace voy::decomp {

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("VOY_DECOMP");
        return !(e && std::strcmp(e, "0") == 0);
    }();
    return on;
}

}
