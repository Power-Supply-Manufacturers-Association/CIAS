// Tests for CiasToNgspiceConverter: atom-brick -> ngspice .subckt.
#include "CiasCircuitConverter.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>

using nlohmann::json;
using namespace CIAS;

#include <catch2/catch_test_macros.hpp>
#define CHECK_MSG(cond, ...) do { INFO(__VA_ARGS__); CHECK(cond); } while (0)
static bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

TEST_CASE("CIAS circuit converter", "[cias]") {
    CiasToNgspiceConverter conv;

    // --- resistor leaf (as produced by ras_to_cias) ---
    json resLeaf = json::parse(R"({
        "name": "resistor",
        "ports": [ {"name":"1"}, {"name":"2"} ],
        "components": [ { "name":"R", "data": { "resistor": { "manufacturerInfo": {
            "name":"ideal", "datasheetInfo": { "part":{"partNumber":"ideal","technology":"thickFilm"},
            "electrical": { "resistance":{"nominal":1000.0}, "tolerance":0.0, "powerRating":0.0 } } } } } } ],
        "connections": [
            { "name":"1", "endpoints":[ {"component":"R","pin":"1"}, {"port":"1"} ] },
            { "name":"2", "endpoints":[ {"component":"R","pin":"2"}, {"port":"2"} ] } ]
    })");
    std::string rs = conv.to_subckt_json(resLeaf);
    std::cout << "--- resistor subckt ---\n" << rs;
    CHECK_MSG(has(rs, ".subckt resistor 1 2"), "resistor: subckt header");
    CHECK_MSG(has(rs, "RR 1 2 1000"), "resistor: R card with value");
    CHECK_MSG(has(rs, ".ends resistor"), "resistor: ends");

    // --- mosfet leaf (sas_to_cias) ---
    json mosLeaf = json::parse(R"({
        "name":"semiconductor",
        "ports":[{"name":"drain"},{"name":"gate"},{"name":"source"}],
        "components":[{"name":"Q","data":{"semiconductor":{"mosfet":{"manufacturerInfo":{"name":"ideal",
            "datasheetInfo":{"part":{"partNumber":"ideal","technology":"Si"},
            "electrical":{"onResistance":0.05,"gateThresholdVoltage":{"nominal":3.0}}}}}}}}],
        "connections":[
            {"name":"drain","endpoints":[{"component":"Q","pin":"drain"},{"port":"drain"}]},
            {"name":"gate","endpoints":[{"component":"Q","pin":"gate"},{"port":"gate"}]},
            {"name":"source","endpoints":[{"component":"Q","pin":"source"},{"port":"source"}]}]
    })");
    std::string ms = conv.to_subckt_json(mosLeaf);
    std::cout << "--- mosfet subckt ---\n" << ms;
    // Switch control is gate-to-GROUND (n+ n- gate 0), not gate-to-source. This is the verified form
    // (the buck/bridge high-side-switch fix): a ground-referenced gate works for both low-side (source
    // at 0 -> identical) and high-side switches, matching MKF's ground-referenced pwm_ctrl. (Earlier
    // pin "gate source" was stale from before that migration.)
    CHECK_MSG(has(ms, "SQ drain source gate 0 SW_Q"), "mosfet: S vc-switch card (gate-to-ground)");
    CHECK_MSG(has(ms, ".model SW_Q SW(Vt=3"), "mosfet: SW model with Vt");
    CHECK_MSG(has(ms, "Ron=0.05"), "mosfet: Ron in model");

    // --- transformer leaf (mas_to_cias): ONE component -> L+L+K ---
    json xfmrLeaf = json::parse(R"({
        "name":"transformer",
        "ports":[{"name":"primary_start"},{"name":"primary_end"},
                 {"name":"secondary1_start"},{"name":"secondary1_end"}],
        "components":[{"name":"T","data":{"magnetic":{},"inputs":{"designRequirements":{
            "magnetizingInductance":{"nominal":1e-3},"turnsRatios":[{"nominal":4.0}]}}}}],
        "connections":[
            {"name":"primary_start","endpoints":[{"component":"T","pin":"primary_start"},{"port":"primary_start"}]},
            {"name":"primary_end","endpoints":[{"component":"T","pin":"primary_end"},{"port":"primary_end"}]},
            {"name":"secondary1_start","endpoints":[{"component":"T","pin":"secondary1_start"},{"port":"secondary1_start"}]},
            {"name":"secondary1_end","endpoints":[{"component":"T","pin":"secondary1_end"},{"port":"secondary1_end"}]}]
    })");
    std::string xs = conv.to_subckt_json(xfmrLeaf);
    std::cout << "--- transformer subckt ---\n" << xs;
    CHECK_MSG(has(xs, "LT_pri primary_start primary_end 0.001"), "xfmr: primary L = Lp");
    CHECK_MSG(has(xs, "LT_sec1 secondary1_start secondary1_end 6.25e-05"), "xfmr: secondary L = Lp/n^2 (1m/16)");
    CHECK_MSG(has(xs, "KT_01 LT_pri LT_sec1 0.9999"), "xfmr: K coupling pri-sec (ideal = 0.9999, well-conditioned for series-inductor topologies)");
}

TEST_CASE("CIAS real diode -> rich .model D (IS@I, CJO, TT, BV)", "[cias][real][diode]") {
    CiasToNgspiceConverter conv;
    json leaf = json::parse(R"({
        "name":"diode",
        "ports":[{"name":"anode"},{"name":"cathode"}],
        "components":[{"name":"D","data":{"semiconductor":{"diode":{"manufacturerInfo":{"name":"ST",
            "datasheetInfo":{"part":{"partNumber":"STPS20H100CT"},
            "electrical":{"forwardVoltage":0.55,"forwardVoltageAt":10,"junctionCapacitance":1.2e-9,
                          "reverseRecoveryTime":2.5e-8,"reverseVoltage":100}}}}}}}],
        "connections":[
            {"name":"anode","endpoints":[{"component":"D","pin":"anode"},{"port":"anode"}]},
            {"name":"cathode","endpoints":[{"component":"D","pin":"cathode"},{"port":"cathode"}]}]
    })");
    std::string s = conv.to_subckt_json(leaf);
    std::cout << "--- real diode subckt ---\n" << s;
    CHECK_MSG(has(s, "CJO=1.2e-09"), "real diode: junctionCapacitance -> CJO");
    CHECK_MSG(has(s, "TT=2.5e-08"), "real diode: reverseRecoveryTime -> TT");
    CHECK_MSG(has(s, "BV=100"), "real diode: reverseVoltage -> BV");
    CHECK_MSG((has(s, "IS=") && !has(s, "IS=1e-14")), "real diode: IS from datasheet Vf@I (not ideal 1e-14)");
}
