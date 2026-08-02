#pragma once

// CIAS circuit data structures + JSON (de)serialization.
//
// A CiasCircuit is the C++ mirror of schemas/CIAS.json: { name, ports, components,
// connections } — a SPICE .subckt analogue. Component data is either an inline PEAS
// document or a URI string into a part-data file; there are NO simulator-specific
// fields (the former LTspice-extraction shapes `spice_params` / `ltspice_declaration`
// were schema-illegal and have been removed — LTspice library extraction lives in the
// Heimdall pipeline as an internal format, see the ABT brief there).

#include <string>
#include <vector>
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
    // Optional brick-level data-provenance trail (CIAS.json `provenance`, ABT #479).
    // Carried OPAQUELY: nothing in this library interprets it, and to_subckt does not
    // need it — but it must survive a from_json/to_json round-trip. It did not before
    // ABT #540: the struct had no member for it, so loading and re-saving a brick
    // SILENTLY dropped the field, and 7,554 connector pin-field bricks carry it as the
    // entire assumption record for a derived artifact. A json rather than a typed
    // mirror, deliberately: an opaque passthrough cannot drift from the PEAS definition
    // CIAS $refs, whereas a typed copy would have to be kept in sync by hand.
    json provenance = nullptr;

    static CiasCircuit from_json(const json& j);
    json to_json() const;
};

} // namespace CIAS
