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

    if (j.contains("spice_params"))
        circuit.spice_params = j.at("spice_params");

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

    if (!spice_params.is_null())
        j["spice_params"] = spice_params;

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

    auto ports = get_ordered_ports(circuit);
    int portCount = ports.size();

    // Determine symbol type based on circuit properties
    std::string symbolType = "BLOCK";
    int width = 64, height = 80;
    bool isPassive = isPassiveComponent(circuit);

    if (isPassive && portCount == 2) {
        // Generate appropriate symbol for passive 2-port components
        symbolType = detectPassiveComponentType(circuit);
    }

    asy << "Version 4\n";
    asy << "SymbolType " << symbolType << "\n";

    // Adjust dimensions based on port count and component type
    if (portCount <= 2) {
        width = 64;
        height = 32;
    } else if (portCount <= 4) {
        width = 96;
        height = 64;
    } else if (portCount <= 6) {
        width = 128;
        height = 96;
    } else {
        width = 160;
        height = 128;
    }

    int rectX0 = 32, rectY0 = 32;
    int rectX1 = rectX0 + width, rectY1 = rectY0 + height;

    // Draw symbol body
    if (isPassive && portCount == 2) {
        // For passive components, draw component-specific shapes
        if (symbolType == "RESISTOR") {
            // Draw resistor zigzag between pins
            asy << "LINE Normal 32 48 40 48\n";
            asy << "LINE Normal 40 40 48 56\n";
            asy << "LINE Normal 48 40 56 56\n";
            asy << "LINE Normal 56 40 64 56\n";
            asy << "LINE Normal 64 48 72 48\n";
        } else if (symbolType == "CAPACITOR") {
            // Draw capacitor (two parallel lines)
            asy << "LINE Normal 32 48 56 48\n";
            asy << "LINE Normal 56 40 56 56\n";
            asy << "LINE Normal 64 40 64 56\n";
            asy << "LINE Normal 64 48 96 48\n";
        } else if (symbolType == "INDUCTOR") {
            // Draw inductor coils
            asy << "ARC Normal 40 40 48 56 40 48 48 40\n";
            asy << "ARC Normal 52 40 60 56 52 48 60 40\n";
            asy << "ARC Normal 64 40 72 56 64 48 72 40\n";
            asy << "LINE Normal 32 48 40 48\n";
            asy << "LINE Normal 72 48 96 48\n";
        } else {
            // Default BLOCK for unknown passive components
            asy << "RECTANGLE Normal " << rectX0 << " " << rectY0 << " "
                << rectX1 << " " << rectY1 << "\n";
        }
    } else {
        // For active/complex components, draw rectangle
        asy << "RECTANGLE Normal " << rectX0 << " " << rectY0 << " "
            << rectX1 << " " << rectY1 << "\n";
    }

    // Calculate pin positions
    int yStart = rectY0 + 16;
    int ySpacing = std::max(16, (rectY1 - rectY0 - 32) / std::max(1, portCount - 1));

    // Place pins
    for (size_t i = 0; i < ports.size(); ++i) {
        int y = yStart + (i * ySpacing);
        // Clamp y to valid range
        y = std::max(rectY0 + 8, std::min(y, rectY1 - 8));

        asy << "PIN 0 " << y << " LEFT 36\n";
        asy << "PINATTR PinName " << ports[i] << "\n";
        asy << "PINATTR SpiceOrder " << (i + 1) << "\n";
    }

    // Add text label with circuit name
    int textY = rectY0 + height / 2;
    asy << "TEXT " << (rectX0 + 4) << " " << textY << " LEFT 2 " << circuit.name << "\n";

    return asy.str();
}

bool CiasToLtspiceConverter::isPassiveComponent(const CiasCircuit& circuit) const {
    // Check if all components are passive (R, C, L)
    for (const auto& comp : circuit.components) {
        auto type = comp.data.value("type", "unknown");
        if (!type.is_string()) return false;

        std::string typeStr = type.get<std::string>();
        if (typeStr != "resistor" && typeStr != "capacitor" && typeStr != "inductor") {
            return false;
        }
    }
    return true;
}

std::string CiasToLtspiceConverter::detectPassiveComponentType(const CiasCircuit& circuit) const {
    // Detect component type based on components in circuit
    bool hasResistor = false, hasCapacitor = false, hasInductor = false;

    for (const auto& comp : circuit.components) {
        auto type = comp.data.value("type", "unknown");
        if (!type.is_string()) continue;

        std::string typeStr = type.get<std::string>();
        if (typeStr == "resistor") hasResistor = true;
        else if (typeStr == "capacitor") hasCapacitor = true;
        else if (typeStr == "inductor") hasInductor = true;
    }

    // Return dominant component type
    if (hasResistor && !hasCapacitor && !hasInductor) return "RESISTOR";
    if (hasCapacitor && !hasResistor && !hasInductor) return "CAPACITOR";
    if (hasInductor && !hasResistor && !hasCapacitor) return "INDUCTOR";

    return "BLOCK";  // Default for mixed or unknown
}

std::string CiasToLtspiceConverter::generate_lib_subcircuit(const CiasCircuit& circuit) const {
    std::ostringstream lib;

    auto ports = get_ordered_ports(circuit);

    lib << ".subckt " << circuit.name;
    for (const auto& port : ports) {
        lib << " " << port;
    }
    lib << "\n";

    // Emit .param blocks required by parametric models (DCbias caps, transformer models, etc.)
    if (circuit.spice_params.is_array()) {
        for (const auto& param_line : circuit.spice_params) {
            if (param_line.is_string())
                lib << param_line.get<std::string>() << "\n";
        }
    }

    for (const auto& comp : circuit.components) {
        // Prefer the original SPICE declaration — it has correct node names and value formatting.
        if (comp.data.contains("ltspice_declaration") &&
            comp.data.at("ltspice_declaration").is_string()) {
            lib << comp.data.at("ltspice_declaration").get<std::string>() << "\n";
            continue;
        }

        // Fallback: reconstruct from type/value (only works when all pins appear in connections).
        if (!comp.data.contains("type") || !comp.data.contains("value"))
            throw std::runtime_error(
                "Component '" + comp.name +
                "' has no ltspice_declaration and no type/value — cannot generate lib");

        auto type  = comp.data.at("type").get<std::string>();
        auto value = comp.data.at("value");

        if      (type == "resistor")  lib << "R";
        else if (type == "capacitor") lib << "C";
        else if (type == "inductor")  lib << "L";
        else                          lib << "X";

        lib << comp.name;

        // Emit nodes from connections (works only when all pins have connection entries)
        auto componentPins = extract_component_pins(circuit);
        auto it = componentPins.find(comp.name);
        if (it != componentPins.end()) {
            for (const auto& pin : it->second) {
                bool found = false;
                for (const auto& conn : circuit.connections) {
                    for (const auto& ep : conn.endpoints) {
                        if (!ep.isPinEndpoint() || ep.component != comp.name || ep.pin != pin)
                            continue;
                        // Prefer port name over internal net name
                        for (const auto& other : conn.endpoints) {
                            if (other.isPortEndpoint()) {
                                lib << " " << other.port;
                                found = true;
                                break;
                            }
                        }
                        if (!found) lib << " " << conn.name;
                        found = true;
                        break;
                    }
                    if (found) break;
                }
            }
        }

        if (value.is_number()) lib << " " << value.get<double>();
        else if (value.is_string()) lib << " " << value.get<std::string>();
        lib << "\n";
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
