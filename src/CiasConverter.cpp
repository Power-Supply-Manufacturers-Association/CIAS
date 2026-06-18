#include "CiasConverter.hpp"
#include <sstream>
#include <algorithm>
#include <stdexcept>
#include <regex>

namespace CIAS {

CiasCircuit CiasCircuit::from_json(const json& j) {
    CiasCircuit circuit;
    circuit.name = j.at("name").get<std::string>();

    for (const auto& port : j.at("ports")) {
        Port p;
        p.name = port.at("name").get<std::string>();
        if (port.contains("description")) {
            p.description = port.at("description").get<std::string>();
        }
        circuit.ports.push_back(p);
    }

    for (const auto& comp : j.at("components")) {
        Component c;
        c.name = comp.at("name").get<std::string>();
        c.data = comp.at("data");
        circuit.components.push_back(c);
    }

    for (const auto& conn : j.at("connections")) {
        Connection c;
        c.name = conn.at("name").get<std::string>();

        for (const auto& ep : conn.at("endpoints")) {
            Endpoint e;
            if (ep.contains("component") && ep.contains("pin")) {
                e.component = ep.at("component").get<std::string>();
                e.pin = ep.at("pin").get<std::string>();
            } else if (ep.contains("port")) {
                e.port = ep.at("port").get<std::string>();
            } else {
                throw std::runtime_error("Invalid endpoint in connection: " + c.name);
            }
            c.endpoints.push_back(e);
        }
        circuit.connections.push_back(c);
    }

    return circuit;
}

json CiasCircuit::to_json() const {
    json j;
    j["name"] = name;

    j["ports"] = json::array();
    for (const auto& port : ports) {
        json p;
        p["name"] = port.name;
        if (!port.description.empty()) {
            p["description"] = port.description;
        }
        j["ports"].push_back(p);
    }

    j["components"] = json::array();
    for (const auto& comp : components) {
        json c;
        c["name"] = comp.name;
        c["data"] = comp.data;
        j["components"].push_back(c);
    }

    j["connections"] = json::array();
    for (const auto& conn : connections) {
        json c;
        c["name"] = conn.name;
        c["endpoints"] = json::array();
        for (const auto& ep : conn.endpoints) {
            json e;
            if (ep.isPinEndpoint()) {
                e["component"] = ep.component;
                e["pin"] = ep.pin;
            } else if (ep.isPortEndpoint()) {
                e["port"] = ep.port;
            }
            c["endpoints"].push_back(e);
        }
        j["connections"].push_back(c);
    }

    return j;
}

CiasToLtspiceConverter::CiasToLtspiceConverter() {}

std::vector<std::string> CiasToLtspiceConverter::get_ordered_ports(const CiasCircuit& circuit) const {
    std::vector<std::string> orderedPorts;
    for (const auto& port : circuit.ports) {
        orderedPorts.push_back(port.name);
    }
    return orderedPorts;
}

std::map<std::string, std::vector<std::string>> CiasToLtspiceConverter::extract_component_pins(
    const CiasCircuit& circuit) const {
    std::map<std::string, std::vector<std::string>> componentPins;

    for (const auto& conn : circuit.connections) {
        for (const auto& ep : conn.endpoints) {
            if (ep.isPinEndpoint()) {
                if (componentPins.find(ep.component) == componentPins.end()) {
                    componentPins[ep.component] = {};
                }
                auto& pins = componentPins[ep.component];
                if (std::find(pins.begin(), pins.end(), ep.pin) == pins.end()) {
                    pins.push_back(ep.pin);
                }
            }
        }
    }

    return componentPins;
}

std::string CiasToLtspiceConverter::generate_asy_symbol(const CiasCircuit& circuit) const {
    std::ostringstream asy;

    asy << "Version 4\n";
    asy << "SymbolType BLOCK\n";

    asy << "RECTANGLE Normal 64 48 304 240\n";

    auto ports = get_ordered_ports(circuit);
    int portCount = ports.size();

    int yStart = 64;
    int ySpacing = std::max(32, 176 / std::max(1, portCount - 1));

    for (size_t i = 0; i < ports.size(); ++i) {
        int y = yStart + (i * ySpacing);
        asy << "PIN 0 " << y << " LEFT 36\n";
        asy << "PINATTR PinName " << ports[i] << "\n";
        asy << "PINATTR SpiceOrder " << (i + 1) << "\n";
    }

    asy << "TEXT 32 144 LEFT 3 " << circuit.name << "\n";

    return asy.str();
}

std::string CiasToLtspiceConverter::generate_lib_subcircuit(const CiasCircuit& circuit) const {
    std::ostringstream lib;

    auto ports = get_ordered_ports(circuit);

    lib << ".subckt " << circuit.name;
    for (const auto& port : ports) {
        lib << " " << port;
    }
    lib << "\n";

    for (const auto& comp : circuit.components) {
        lib << "* Component: " << comp.name << "\n";
        if (comp.data.is_object()) {
            if (comp.data.contains("type")) {
                auto type = comp.data.at("type").get<std::string>();

                if (comp.data.contains("value")) {
                    auto value = comp.data.at("value");

                    if (type == "resistor" || type == "Resistor" || type == "R") {
                        lib << "R" << comp.name << " ";
                    } else if (type == "capacitor" || type == "Capacitor" || type == "C") {
                        lib << "C" << comp.name << " ";
                    } else if (type == "inductor" || type == "Inductor" || type == "L") {
                        lib << "L" << comp.name << " ";
                    } else {
                        lib << "X" << comp.name << " ";
                    }

                    auto componentPins = extract_component_pins(circuit);
                    if (componentPins.find(comp.name) != componentPins.end()) {
                        for (const auto& pin : componentPins[comp.name]) {
                            for (const auto& conn : circuit.connections) {
                                for (const auto& ep : conn.endpoints) {
                                    if (ep.isPinEndpoint() && ep.component == comp.name &&
                                        ep.pin == pin) {
                                        for (const auto& epOther : conn.endpoints) {
                                            if (epOther.isPortEndpoint()) {
                                                lib << epOther.port << " ";
                                                break;
                                            } else if (epOther.isPinEndpoint()) {
                                                lib << conn.name << " ";
                                                break;
                                            }
                                        }
                                        goto next_pin;
                                    }
                                }
                            }
                            next_pin:;
                        }
                    }

                    if (type == "resistor" || type == "Resistor" || type == "R" ||
                        type == "capacitor" || type == "Capacitor" || type == "C" ||
                        type == "inductor" || type == "Inductor" || type == "L") {

                        if (value.is_number()) {
                            lib << value.get<double>();
                        } else if (value.is_string()) {
                            lib << value.get<std::string>();
                        }
                    }

                    lib << "\n";
                }
            }
        }
    }

    lib << ".ends " << circuit.name << "\n";

    return lib.str();
}

std::string CiasToLtspiceConverter::to_asy(const CiasCircuit& circuit) const {
    return generate_asy_symbol(circuit);
}

std::string CiasToLtspiceConverter::to_lib(const CiasCircuit& circuit) const {
    return generate_lib_subcircuit(circuit);
}

std::pair<std::string, std::string> CiasToLtspiceConverter::convert(const CiasCircuit& circuit) const {
    return {to_asy(circuit), to_lib(circuit)};
}

std::pair<std::string, std::string> CiasToLtspiceConverter::convert_json(const json& ciasJson) const {
    auto circuit = CiasCircuit::from_json(ciasJson);
    return convert(circuit);
}

LtspiceToRawConverter::LtspiceToRawConverter() {}

std::vector<std::string> LtspiceToRawConverter::extract_pins_from_asy(const std::string& asyContent) const {
    std::vector<std::string> pins;
    std::istringstream iss(asyContent);
    std::string line;

    while (std::getline(iss, line)) {
        if (line.find("PINATTR PinName") != std::string::npos) {
            size_t namePos = line.find("PINATTR PinName");
            if (namePos != std::string::npos) {
                std::string name = line.substr(namePos + 15);
                name.erase(0, name.find_first_not_of(" \t"));
                name.erase(name.find_last_not_of(" \t") + 1);
                pins.push_back(name);
            }
        }
    }

    return pins;
}

std::vector<std::string> LtspiceToRawConverter::extract_subckt_ports(const std::string& libContent) const {
    std::vector<std::string> ports;
    std::istringstream iss(libContent);
    std::string line;

    while (std::getline(iss, line)) {
        if (line.find(".subckt") == 0) {
            std::istringstream tokens(line);
            std::string token;

            tokens >> token;

            while (tokens >> token) {
                if (token[0] != '.') {
                    ports.push_back(token);
                }
            }
            break;
        }
    }

    return ports;
}

std::map<std::string, std::string> LtspiceToRawConverter::extract_component_declarations(
    const std::string& libContent) const {
    std::map<std::string, std::string> components;
    std::istringstream iss(libContent);
    std::string line;

    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '*' || line[0] == '.') {
            continue;
        }

        char compType = line[0];
        if (compType == 'R' || compType == 'C' || compType == 'L' || compType == 'X') {
            size_t spacePos = line.find_first_of(" \t");
            if (spacePos != std::string::npos) {
                std::string compName = line.substr(0, spacePos);
                components[compName] = line;
            }
        }
    }

    return components;
}

json LtspiceToRawConverter::asy_to_json(const std::string& asyContent) const {
    json j;
    auto pins = extract_pins_from_asy(asyContent);
    j["pins"] = pins;
    return j;
}

json LtspiceToRawConverter::lib_to_json(const std::string& libContent) const {
    json j;
    auto ports = extract_subckt_ports(libContent);
    auto components = extract_component_declarations(libContent);

    j["ports"] = ports;
    j["components"] = json::object();
    for (const auto& [name, declaration] : components) {
        j["components"][name] = declaration;
    }

    return j;
}

CiasCircuit LtspiceToRawConverter::from_ltspice_files(
    const std::string& asyContent,
    const std::string& libContent) const {

    CiasCircuit circuit;

    auto pins = extract_pins_from_asy(asyContent);
    for (const auto& pin : pins) {
        circuit.ports.push_back({pin, ""});
    }

    auto components = extract_component_declarations(libContent);
    for (const auto& [name, _] : components) {
        circuit.components.push_back({name, json::object()});
    }

    return circuit;
}

json LtspiceToRawConverter::from_ltspice_files_json(
    const std::string& asyContent,
    const std::string& libContent) const {

    auto circuit = from_ltspice_files(asyContent, libContent);
    return circuit.to_json();
}

} // namespace CIAS
