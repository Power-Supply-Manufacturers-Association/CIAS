#pragma once

// CiasCircuitConverter — "CIAS converts a circuit to ngspice / LTspice / ...".
//
// Input: a CIAS brick whose components are ideal ATOM PEAS docs (the output of the per-family
// to_cias generators, possibly merged by the TAS assembler). The converter dispatches on each
// component's PEAS discriminator and emits the SPICE element card(s):
//   resistor      -> R card                (pins 1,2)
//   capacitor     -> C card                (pins 1,2)
//   semiconductor mosfet -> S (vc-switch) + .model SW   (pins drain,gate,source)
//   semiconductor diode  -> D + .model D                (pins anode,cathode)
//   magnetic      -> L per winding + K coupling (the one multi-winding component expanded here;
//                    pins primary_start/end, secondary{i}_start/end)
// Nodes are resolved from the brick's connections (a net exposed at a port takes the port name).
//
// This is the ngspice emitter (Phase 2). LTspice/PSIM/Simba/NL5 share the dispatch; ngspice is the
// runnable target. No silent fallbacks: an unwired pin / unknown discriminator / missing value throws.

#include "CiasConverter.hpp"   // CiasCircuit struct + from_json
#include <string>

namespace CIAS {

class CiasToNgspiceConverter {
public:
    // Emit ".subckt <name> <ports...> ... .ends" for an atom-brick.
    std::string to_subckt(const CiasCircuit& circuit) const;
    std::string to_subckt_json(const json& ciasJson) const;

    // Emit just the element cards (no .subckt wrapper) — used by the deck assembler that supplies
    // its own testbench. Node names are the brick's nets (ports included).
    std::string to_cards(const CiasCircuit& circuit) const;
};

} // namespace CIAS
