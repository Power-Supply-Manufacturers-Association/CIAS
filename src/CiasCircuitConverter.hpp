#pragma once

// CiasCircuitConverter — converts a CIAS circuit to any supported circuit simulator's netlist format.
//
// Supported targets:
//   Ngspice  — PEAS-atom rendering (resistor/capacitor/magnetic/semiconductor/analog)
//              with ngspice SPICE syntax.
//   Ltspice  — if any component carries a 'ltspice_declaration' field, emits a passthrough
//              ".subckt NAME ports [PARAMS:...]\n<original declarations>\n.ends" (round-trip mode).
//              Otherwise: PEAS-atom rendering with LTspice syntax (only the behavioral ternary
//              in the integrator analog block differs from ngspice).
//   NL5, Simba, Plecs — throw "not yet implemented" (stubs for future backends).
//
// Input: a CIAS circuit whose components are either:
//   (a) PEAS-enriched atoms (resistor/capacitor/magnetic/semiconductor/analog discriminators) — MAS pipeline
//   (b) LTspice-extracted circuits (ltspice_declaration on each component) — WE library extraction
//
// Node resolution (for PEAS-atom mode): via the brick's connections graph (port nets take the port name).
// No silent fallbacks: unknown discriminator / unwired pin / missing value throws.

#include "CiasConverter.hpp"
#include <string>
#include <memory>
#include <vector>

namespace CIAS {

enum class CircuitSimulator { Ngspice, Ltspice, NL5, Simba, Plecs };

// Internal SPICE dialect — kept for the emit_peas_cards / ternary dispatch.
enum class SpiceDialect { Ngspice, Ltspice };

class CiasCircuitConverter {
public:
    explicit CiasCircuitConverter(CircuitSimulator target = CircuitSimulator::Ngspice);

    // Factory — returns a heap-allocated converter for the requested simulator.
    static std::shared_ptr<CiasCircuitConverter> create(CircuitSimulator target);

    // Emit ".subckt <name> <ports> [PARAMS: ...]\n<element cards>\n.ends <name>".
    // For Ltspice + ltspice_declaration present: passthrough with PARAMS: header.
    // For Ngspice/Ltspice without ltspice_declaration: PEAS-atom rendering.
    // For NL5/Simba/Plecs: throws runtime_error.
    std::string to_subckt(const CiasCircuit& circuit) const;
    std::string to_subckt_json(const json& ciasJson) const;

    // Element cards only (no .subckt wrapper) — for assembler decks.
    // Valid for Ngspice and Ltspice PEAS-atom mode only; throws for others.
    std::string to_cards(const CiasCircuit& circuit) const;

private:
    CircuitSimulator target_;

    SpiceDialect spice_dialect() const;
    bool has_ltspice_declarations(const CiasCircuit& circuit) const;
    std::string emit_peas_cards(const CiasCircuit& circuit, SpiceDialect dialect) const;
    std::string emit_ltspice_passthrough(const CiasCircuit& circuit) const;
};

// Backward-compatible alias — existing code using CiasToNgspiceConverter keeps compiling.
// CiasCircuitConverter(CircuitSimulator::Ngspice) is the default, so the zero-arg constructor
// and to_cards()/to_subckt() signatures behave identically to the old class.
using CiasToNgspiceConverter = CiasCircuitConverter;

// Structural validator — returns human-readable problems ([] if the brick is well-formed).
// Complements the JSON-Schema (Python) validator with graph-level checks the schema cannot
// express: unique names, every pinEndpoint references a real component, every portEndpoint
// references a declared port, connections have >=2 endpoints, each component carries exactly
// one known discriminator (or a URI string).
std::vector<std::string> validate_cias_structure(const CiasCircuit& circuit);

} // namespace CIAS
