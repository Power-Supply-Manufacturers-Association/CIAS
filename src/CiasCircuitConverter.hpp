#pragma once

// CiasCircuitConverter — converts a CIAS circuit to a circuit simulator's netlist format.
//
// Supported targets:
//   Ngspice / Ltspice — PEAS-atom rendering (resistor / capacitor / magnetic / semiconductor /
//                       analog / behavioral discriminators). The two dialects differ in:
//                       behavioral ternary ((c)?(a):(b) vs if(c,a,b)), flux/charge realization
//                       (B+ddt()+sense vs native Flux=/Q= attributes), Chan core (closed-form
//                       B-H behavioral model vs native Hc/Bs/Br inductor), K-coefficient cap
//                       (ngspice cannot solve k==1), switch model params (Von/Voff vs Vt/Vh),
//                       and pwl()->table() rewriting in controlled sources.
//   NL5, Simba, Plecs — throw "not yet implemented" (stubs for future backends).
//
// Input: a CIAS circuit whose components are PEAS documents (inline) — the LTspice-extraction
// passthrough (`ltspice_declaration` / `spice_params`) was removed: those shapes are illegal
// against CIAS.json and live on only as an internal format of the Heimdall extraction pipeline.
//
// Node resolution: via the brick's connections graph (a net exposed at a port takes the port
// name; a net at TWO ports is an error). No silent fallbacks: unknown discriminator, unknown
// pin name, unwired required value, or missing behavioral parameters throw.

#include "CiasConverter.hpp"
#include <string>
#include <memory>
#include <vector>

namespace CIAS {

enum class CircuitSimulator { Ngspice, Ltspice, NL5, Simba, Plecs };

// Internal SPICE dialect — kept for the emit_peas_cards dispatch.
enum class SpiceDialect { Ngspice, Ltspice };

class CiasCircuitConverter {
public:
    explicit CiasCircuitConverter(CircuitSimulator target = CircuitSimulator::Ngspice);

    // Factory — returns a heap-allocated converter for the requested simulator.
    static std::shared_ptr<CiasCircuitConverter> create(CircuitSimulator target);

    // Emit ".subckt <name> <ports>\n<element cards>\n.ends <name>".
    // Ngspice/Ltspice: PEAS-atom rendering. NL5/Simba/Plecs: throws runtime_error.
    std::string to_subckt(const CiasCircuit& circuit) const;
    std::string to_subckt_json(const json& ciasJson) const;

    // Element cards only (no .subckt wrapper) — for assembler decks.
    std::string to_cards(const CiasCircuit& circuit) const;

private:
    CircuitSimulator target_;

    SpiceDialect spice_dialect() const;
    std::string emit_peas_cards(const CiasCircuit& circuit, SpiceDialect dialect) const;
};

// Backward-compatible alias — existing code using CiasToNgspiceConverter keeps compiling.
using CiasToNgspiceConverter = CiasCircuitConverter;

// Structural validator — returns human-readable problems ([] if the brick is well-formed).
// Complements the JSON-Schema (Python) validator with graph-level checks the schema cannot
// express: unique names, every pinEndpoint references a real component, every portEndpoint
// references a declared port, connections have >=2 endpoints, each component carries exactly
// one known discriminator (or a URI string).
std::vector<std::string> validate_cias_structure(const CiasCircuit& circuit);

} // namespace CIAS
