#pragma once

#include <string>
#include <vector>
#include <map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace CIAS {

struct Port {
    std::string name;
    std::string description;
};

struct Component {
    std::string name;
    json data;
};

struct Endpoint {
    std::string component;
    std::string pin;
    std::string port;

    bool isPinEndpoint() const { return !component.empty() && !pin.empty(); }
    bool isPortEndpoint() const { return !port.empty(); }
};

struct Connection {
    std::string name;
    std::vector<Endpoint> endpoints;
};

struct CiasCircuit {
    std::string name;
    std::vector<Port> ports;
    std::vector<Component> components;
    std::vector<Connection> connections;
    json spice_params;   // raw .param lines from the original .subckt (may be null)

    static CiasCircuit from_json(const json& j);
    json to_json() const;
};

class CiasToLtspiceConverter {
public:
    CiasToLtspiceConverter();

    std::string to_asy(const CiasCircuit& circuit) const;
    std::string to_lib(const CiasCircuit& circuit) const;

    std::pair<std::string, std::string> convert(const CiasCircuit& circuit) const;
    std::pair<std::string, std::string> convert_json(const json& ciasJson) const;

private:
    std::string generate_asy_symbol(const CiasCircuit& circuit) const;
    std::string generate_lib_subcircuit(const CiasCircuit& circuit) const;

    std::map<std::string, std::vector<std::string>> extract_component_pins(
        const CiasCircuit& circuit) const;

    std::vector<std::string> get_ordered_ports(const CiasCircuit& circuit) const;

    bool isPassiveComponent(const CiasCircuit& circuit) const;
    std::string detectPassiveComponentType(const CiasCircuit& circuit) const;
};

class LtspiceToRawConverter {
public:
    LtspiceToRawConverter();

    json asy_to_json(const std::string& asyContent) const;
    json lib_to_json(const std::string& libContent) const;

    CiasCircuit from_ltspice_files(
        const std::string& asyContent,
        const std::string& libContent) const;

    json from_ltspice_files_json(
        const std::string& asyContent,
        const std::string& libContent) const;

private:
    std::vector<std::string> extract_pins_from_asy(const std::string& asyContent) const;
    std::vector<std::string> extract_subckt_ports(const std::string& libContent) const;
    std::map<std::string, std::string> extract_component_declarations(
        const std::string& libContent) const;
};

} // namespace CIAS
