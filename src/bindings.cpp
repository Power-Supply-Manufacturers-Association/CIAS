#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11_json/pybind11_json.hpp>
#include "CiasConverter.hpp"

namespace py = pybind11;
using json = nlohmann::json;

PYBIND11_MODULE(PyCIAS, m) {
    m.doc() = "CIAS (Circuit Agnostic Structure) Python bindings for circuit conversion";

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

    py::class_<CIAS::CiasToLtspiceConverter>(m, "CiasToLtspiceConverter")
        .def(py::init<>())
        .def("to_asy", &CIAS::CiasToLtspiceConverter::to_asy)
        .def("to_lib", &CIAS::CiasToLtspiceConverter::to_lib)
        .def("convert", &CIAS::CiasToLtspiceConverter::convert)
        .def("convert_json", &CIAS::CiasToLtspiceConverter::convert_json,
             py::arg("cias_json"),
             "Convert a CIAS JSON object to LTspice .asy and .lib file contents");

    py::class_<CIAS::LtspiceToRawConverter>(m, "LtspiceToRawConverter")
        .def(py::init<>())
        .def("asy_to_json", &CIAS::LtspiceToRawConverter::asy_to_json)
        .def("lib_to_json", &CIAS::LtspiceToRawConverter::lib_to_json)
        .def("from_ltspice_files", &CIAS::LtspiceToRawConverter::from_ltspice_files)
        .def("from_ltspice_files_json", &CIAS::LtspiceToRawConverter::from_ltspice_files_json,
             py::arg("asy_content"),
             py::arg("lib_content"),
             "Convert LTspice .asy and .lib files to CIAS JSON format");

    m.def("convert_cias_to_ltspice", [](const json& ciasJson) {
        CIAS::CiasToLtspiceConverter converter;
        return converter.convert_json(ciasJson);
    }, py::arg("cias_json"), "Convert CIAS JSON to LTspice files (returns tuple of asy, lib)");

    m.def("convert_ltspice_to_cias", [](const std::string& asyContent, const std::string& libContent) {
        CIAS::LtspiceToRawConverter converter;
        return converter.from_ltspice_files_json(asyContent, libContent);
    }, py::arg("asy_content"), py::arg("lib_content"), "Convert LTspice files to CIAS JSON");
}
