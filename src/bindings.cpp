#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11_json/pybind11_json.hpp>
#include "CiasConverter.hpp"
#include "CiasCircuitConverter.hpp"

namespace py = pybind11;
using json = nlohmann::json;

PYBIND11_MODULE(PyCIAS, m) {
    m.doc() = "CIAS (Circuit Agnostic Structure) Python bindings for circuit conversion";

    // ─── Data structures ────────────────────────────────────────────────────

    py::class_<CIAS::Port>(m, "Port")
        .def(py::init<>())
        .def_readwrite("name", &CIAS::Port::name)
        .def_readwrite("description", &CIAS::Port::description);

    py::class_<CIAS::Endpoint>(m, "Endpoint")
        .def(py::init<>())
        .def_readwrite("component", &CIAS::Endpoint::component)
        .def_readwrite("pin", &CIAS::Endpoint::pin)
        .def_readwrite("port", &CIAS::Endpoint::port)
        .def("is_pin_endpoint", &CIAS::Endpoint::isPinEndpoint)
        .def("is_port_endpoint", &CIAS::Endpoint::isPortEndpoint);

    py::class_<CIAS::Component>(m, "Component")
        .def(py::init<>())
        .def_readwrite("name", &CIAS::Component::name)
        .def_readwrite("data", &CIAS::Component::data);

    py::class_<CIAS::Connection>(m, "Connection")
        .def(py::init<>())
        .def_readwrite("name", &CIAS::Connection::name)
        .def_readwrite("endpoints", &CIAS::Connection::endpoints);

    py::class_<CIAS::CiasCircuit>(m, "CiasCircuit")
        .def(py::init<>())
        .def_readwrite("name", &CIAS::CiasCircuit::name)
        .def_readwrite("ports", &CIAS::CiasCircuit::ports)
        .def_readwrite("components", &CIAS::CiasCircuit::components)
        .def_readwrite("connections", &CIAS::CiasCircuit::connections)
        .def_static("from_json", &CIAS::CiasCircuit::from_json)
        .def("to_json", &CIAS::CiasCircuit::to_json);

    // ─── Multi-simulator circuit converter ─────────────────────────────────

    py::enum_<CIAS::CircuitSimulator>(m, "CircuitSimulator")
        .value("Ngspice", CIAS::CircuitSimulator::Ngspice)
        .value("Ltspice", CIAS::CircuitSimulator::Ltspice)
        .value("NL5",     CIAS::CircuitSimulator::NL5)
        .value("Simba",   CIAS::CircuitSimulator::Simba)
        .value("Plecs",   CIAS::CircuitSimulator::Plecs)
        .export_values();

    py::class_<CIAS::CiasCircuitConverter>(m, "CiasCircuitConverter")
        .def(py::init<CIAS::CircuitSimulator>(), py::arg("target") = CIAS::CircuitSimulator::Ngspice,
             "Create a converter for the given simulator target.")
        .def_static("create", &CIAS::CiasCircuitConverter::create, py::arg("target"),
             "Factory: return a shared converter for the given simulator target.")
        .def("to_subckt", &CIAS::CiasCircuitConverter::to_subckt, py::arg("circuit"),
             "Emit .subckt/.ends from a CiasCircuit. "
             "Ltspice target: passthrough via ltspice_declaration if present, else PEAS atoms. "
             "Ngspice: PEAS atoms. NL5/Simba/Plecs: raises RuntimeError (not yet implemented).")
        .def("to_subckt_json", &CIAS::CiasCircuitConverter::to_subckt_json, py::arg("cias_json"),
             "Same as to_subckt() but accepts a CIAS JSON dict instead of a CiasCircuit object.")
        .def("to_cards", &CIAS::CiasCircuitConverter::to_cards, py::arg("circuit"),
             "Emit element cards only (no .subckt wrapper) for assembler decks. "
             "Valid for Ngspice and Ltspice PEAS-atom mode only.");

    // Backward-compat alias: CiasToNgspiceConverter is now CiasCircuitConverter.
    // Old code using CiasToNgspiceConverter().to_subckt_json(j) still works.
    m.attr("CiasToNgspiceConverter") = m.attr("CiasCircuitConverter");

    // ─── LTspice symbol+lib exporter (for MAS-designed components) ─────────

    py::class_<CIAS::CiasToLtspiceConverter>(m, "CiasToLtspiceConverter")
        .def(py::init<>())
        .def("to_asy", &CIAS::CiasToLtspiceConverter::to_asy,
             "Emit .asy symbol file content for the given CIAS circuit.")
        .def("to_lib", &CIAS::CiasToLtspiceConverter::to_lib,
             "Emit .lib subcircuit file content for the given CIAS circuit.")
        .def("convert", &CIAS::CiasToLtspiceConverter::convert,
             "Return (asy_content, lib_content) tuple.")
        .def("convert_json", &CIAS::CiasToLtspiceConverter::convert_json,
             py::arg("cias_json"),
             "Convert a CIAS JSON dict to (asy_content, lib_content).");

    py::class_<CIAS::LtspiceToRawConverter>(m, "LtspiceToRawConverter")
        .def(py::init<>())
        .def("asy_to_json", &CIAS::LtspiceToRawConverter::asy_to_json)
        .def("lib_to_json", &CIAS::LtspiceToRawConverter::lib_to_json)
        .def("from_ltspice_files", &CIAS::LtspiceToRawConverter::from_ltspice_files)
        .def("from_ltspice_files_json", &CIAS::LtspiceToRawConverter::from_ltspice_files_json,
             py::arg("asy_content"), py::arg("lib_content"),
             "Convert LTspice .asy and .lib files to CIAS JSON format.");

    // ─── Convenience free functions ─────────────────────────────────────────

    m.def("convert_cias_to_simulator",
        [](const json& ciasJson, CIAS::CircuitSimulator target) {
            CIAS::CiasCircuitConverter conv(target);
            return conv.to_subckt_json(ciasJson);
        },
        py::arg("cias_json"), py::arg("target") = CIAS::CircuitSimulator::Ngspice,
        "Convert a CIAS JSON dict to a .subckt string for the given simulator target.");

    m.def("convert_cias_to_ngspice",
        [](const json& ciasJson) {
            return CIAS::CiasCircuitConverter(CIAS::CircuitSimulator::Ngspice).to_subckt_json(ciasJson);
        },
        py::arg("cias_json"),
        "Convert a CIAS JSON dict to an ngspice .subckt string (PEAS-atom rendering).");

    m.def("convert_cias_to_ltspice_subckt",
        [](const json& ciasJson) {
            return CIAS::CiasCircuitConverter(CIAS::CircuitSimulator::Ltspice).to_subckt_json(ciasJson);
        },
        py::arg("cias_json"),
        "Convert a CIAS JSON dict to an LTspice .subckt string. "
        "Uses ltspice_declaration passthrough if present; PEAS-atom rendering otherwise.");

    m.def("convert_cias_to_ltspice",
        [](const json& ciasJson) {
            CIAS::CiasToLtspiceConverter converter;
            return converter.convert_json(ciasJson);
        },
        py::arg("cias_json"),
        "Convert a CIAS JSON dict to LTspice symbol+lib files (returns (asy, lib) tuple). "
        "For MAS-designed components; use convert_cias_to_ltspice_subckt() for library circuits.");

    m.def("convert_ltspice_to_cias",
        [](const std::string& asyContent, const std::string& libContent) {
            CIAS::LtspiceToRawConverter converter;
            return converter.from_ltspice_files_json(asyContent, libContent);
        },
        py::arg("asy_content"), py::arg("lib_content"),
        "Convert LTspice .asy and .lib files to CIAS JSON format.");

    // ─── Structural validator ───────────────────────────────────────────────
    m.def("validate_cias_structure",
        [](const CIAS::CiasCircuit& circuit) {
            return CIAS::validate_cias_structure(circuit);
        },
        py::arg("circuit"),
        "Return a list of structural problems ([] if well-formed): unique names, every "
        "pin/port endpoint resolves, connections have >=2 endpoints, one discriminator per "
        "component. Graph-level checks complementing the Python JSON-Schema validator.");

    m.def("validate_cias_structure_json",
        [](const json& ciasJson) {
            return CIAS::validate_cias_structure(CIAS::CiasCircuit::from_json(ciasJson));
        },
        py::arg("cias_json"),
        "Same as validate_cias_structure() but accepts a CIAS JSON dict.");
}
