#include "recomp/dispatch.hpp"

#include <algorithm>
#include <cstdio>

#include "host/fail.hpp"
#include "host/process.hpp"
#include "recomp/x86ops.hpp"

namespace voy::recomp {

namespace {

const DispatchTables& tables(const Cpu& c) { return static_cast<const host::Process*>(c.process)->dispatch(); }

[[gnu::noinline, gnu::cold]] void import_unbound(Cpu& c, std::uint32_t target) {
    const auto& t = tables(c);
    const std::uint32_t k = (target - ImportBase) / 16;
    if ((target - ImportBase) % 16 == 0 && k < t.imports.size()) {
        char what[200];
        std::snprintf(what, sizeof what, "import %s has no handler (called from %08X)",
                      t.imports[k].name ? t.imports[k].name : "?", ld32(c.mem, c.r[Cpu::ESP]));
        host::fail_closed(host::Unported::Import, t.imports[k].got, what);
    }
    host::fail_closed(host::Unported::Import, target, "import slot without a handler");
}

}  // namespace

GuestFn find_entry(const Cpu& c, std::uint32_t target) {
    const auto& e = tables(c).entries;
    auto it = std::lower_bound(e.begin(), e.end(), target, [](const GuestEntry& g, std::uint32_t a) { return g.address < a; });
    return it != e.end() && it->address == target ? it->fn : nullptr;
}

void call_indirect(Cpu& c, std::uint32_t target) {
    if (target >= ImportBase) {
        const auto& t = tables(c);
        const std::uint32_t k = (target - ImportBase) / 16;
        if ((target - ImportBase) % 16 == 0 && k < t.imports.size() && t.imports[k].handler) return t.imports[k].handler(c);
        return import_unbound(c, target);
    }
    if (GuestFn fn = find_entry(c, target)) return fn(c);
    host::fail_closed(host::Unported::Function, target, "indirect call to an address with no translated function");
}

std::uint32_t call_guest(Cpu& c, std::uint32_t target, std::initializer_list<std::uint32_t> args) {
    const std::uint32_t saved = c.r[Cpu::ESP];
    std::uint32_t esp = saved;
    for (auto it = args.end(); it != args.begin();) {
        esp -= 4;
        st32(c.mem, esp, *--it);
    }
    esp -= 4;
    st32(c.mem, esp, 0);  // return address: translated code returns natively
    c.r[Cpu::ESP] = esp;
    call_indirect(c, target);
    const std::uint32_t eax = c.r[Cpu::EAX];
    c.r[Cpu::ESP] = saved;
    return eax;
}

}
