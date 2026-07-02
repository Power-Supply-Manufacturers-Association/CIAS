#include "CiasConverter.hpp"
#include <stdexcept>

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

} // namespace CIAS
