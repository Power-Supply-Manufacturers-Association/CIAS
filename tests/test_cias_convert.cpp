// Catch2 tests for CiasCircuitConverter — the PEAS-atom -> SPICE emitter.
//
// Contract under test (post-2026-07 rewrite):
//   * ideal R/C values come from inputs.designRequirements (resolve_dimensional_values order)
//   * semiconductors emit their spiceModel verbatim (no datasheet-derived fabrication)
//   * flux/charge ngspice sense polarity: port current ENTERS terminal 1 (H4 regression)
//   * analog blocks throw without a behavioral model (no fabricated ideal parts)
//   * source/switch natures emit V/I and S+SW cards
//   * VDMOS models get 3-node M cards; sec-sec coupling k = k_i * k_j
//   * unknown pins / double-port nets / corrupt magnetics data throw

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>
#include "CiasCircuitConverter.hpp"

using json = nlohmann::json;
using Catch::Matchers::ContainsSubstring;

namespace {

json resistor_atom(double r) {
    return {{"resistor", json::object()},
            {"inputs", {{"designRequirements", {{"deviceType", "resistor"}, {"resistance", {{"nominal", r}}}}}}}};
}

json capacitor_atom(double c) {
    return {{"capacitor", json::object()},
            {"inputs", {{"designRequirements", {{"capacitance", {{"nominal", c}}}}}}}};
}

// One net landing on a component pin and exposing a same-named brick port.
json pin_port_net(const std::string& net, const std::string& comp,
                  const std::string& pin, const std::string& port) {
    return {{"name", net},
            {"endpoints", json::array({{{"component", comp}, {"pin", pin}}, {{"port", port}}})}};
}

json rc_circuit() {
    return {
        {"name", "rc"},
        {"ports", json::array({{{"name", "in"}}, {{"name", "out"}}, {{"name", "gnd"}}})},
        {"components", json::array({{{"name", "R1"}, {"data", resistor_atom(1000.0)}},
                                    {{"name", "C1"}, {"data", capacitor_atom(1e-6)}}})},
        {"connections", json::array({
            pin_port_net("nin", "R1", "1", "in"),
            {{"name", "mid"}, {"endpoints", json::array({{{"component", "R1"}, {"pin", "2"}},
                                                          {{"component", "C1"}, {"pin", "1"}},
                                                          {{"port", "out"}}})}},
            pin_port_net("ngnd", "C1", "2", "gnd"),
        })},
    };
}

std::string emit(const json& circuit, CIAS::CircuitSimulator target = CIAS::CircuitSimulator::Ngspice) {
    return CIAS::CiasCircuitConverter(target).to_subckt_json(circuit);
}

} // namespace

TEST_CASE("RC brick emits R and C cards from designRequirements", "[cias]") {
    const std::string net = emit(rc_circuit());
    CHECK_THAT(net, ContainsSubstring(".subckt rc in out gnd"));
    CHECK_THAT(net, ContainsSubstring("RR1 in out 1000"));
    CHECK_THAT(net, ContainsSubstring("CC1 out gnd 1e-06"));
    CHECK_THAT(net, ContainsSubstring(".ends rc"));
}

TEST_CASE("dimensionWithTolerance resolves min/max mean when nominal absent", "[cias]") {
    json c = rc_circuit();
    c["components"][0]["data"]["inputs"]["designRequirements"]["resistance"] =
        {{"minimum", 900.0}, {"maximum", 1100.0}};
    CHECK_THAT(emit(c), ContainsSubstring("RR1 in out 1000"));
}

TEST_CASE("0-ohm resistor maps to the negligible-value realization", "[cias]") {
    json c = rc_circuit();
    c["components"][0]["data"]["inputs"]["designRequirements"]["resistance"] = {{"nominal", 0.0}};
    CHECK_THAT(emit(c), ContainsSubstring("RR1 in out 1e-12"));
}

TEST_CASE("diode emits its spiceModel verbatim — no datasheet fabrication", "[cias]") {
    json diode = {{"semiconductor", {{"diode", {
                      {"spiceModel", {{"modelType", "D"},
                                      {"parameters", {{"Is", 1e-9}, {"N", 1.8}}}}}}}}},
                  {"inputs", {{"designRequirements", json::object()}}}};
    json c = {{"name", "d1"},
              {"ports", json::array({{{"name", "a"}}, {{"name", "k"}}})},
              {"components", json::array({{{"name", "D5"}, {"data", diode}}})},
              {"connections", json::array({pin_port_net("na", "D5", "anode", "a"),
                                           pin_port_net("nk", "D5", "cathode", "k")})}};
    const std::string net = emit(c);
    CHECK_THAT(net, ContainsSubstring("DD5 a k MODEL_D5"));
    CHECK_THAT(net, ContainsSubstring(".model MODEL_D5 D(Is=1e-09 N=1.8 )"));
}

TEST_CASE("semiconductor without spiceModel throws", "[cias]") {
    json diode = {{"semiconductor", {{"diode", json::object()}}},
                  {"inputs", {{"designRequirements", json::object()}}}};
    json c = {{"name", "d1"},
              {"ports", json::array({{{"name", "a"}}, {{"name", "k"}}})},
              {"components", json::array({{{"name", "D1"}, {"data", diode}}})},
              {"connections", json::array({pin_port_net("na", "D1", "anode", "a"),
                                           pin_port_net("nk", "D1", "cathode", "k")})}};
    CHECK_THROWS_WITH(emit(c), ContainsSubstring("no spiceModel"));
}

TEST_CASE("VDMOS mosfet emits a 3-node M card; other models 4-node", "[cias]") {
    auto mosfet_with = [](const std::string& mtype) {
        json m = {{"semiconductor", {{"mosfet", {{"spiceModel", {{"modelType", mtype}}}}}}},
                  {"inputs", {{"designRequirements", json::object()}}}};
        return json{{"name", "sw"},
                    {"ports", json::array({{{"name", "d"}}, {{"name", "g"}}, {{"name", "s"}}})},
                    {"components", json::array({{{"name", "Q1"}, {"data", m}}})},
                    {"connections", json::array({pin_port_net("nd", "Q1", "drain", "d"),
                                                 pin_port_net("ng", "Q1", "gate", "g"),
                                                 pin_port_net("ns", "Q1", "source", "s")})}};
    };
    CHECK_THAT(emit(mosfet_with("VDMOS")), ContainsSubstring("MQ1 d g s MODEL_Q1"));
    CHECK_THAT(emit(mosfet_with("NMOS")), ContainsSubstring("MQ1 d g s s MODEL_Q1"));
}

TEST_CASE("flux behavioral: ngspice sense is port-node-first (H4 sign regression)", "[cias]") {
    json beh = {{"behavioral", {{"nature", "flux"}, {"expression", "0.001*atan(i)"}}},
                {"inputs", {{"designRequirements", json::object()}}}};
    json c = {{"name", "sat"},
              {"ports", json::array({{{"name", "a"}}, {{"name", "b"}}})},
              {"components", json::array({{{"name", "L1"}, {"data", beh}}})},
              {"connections", json::array({pin_port_net("na", "L1", "1", "a"),
                                           pin_port_net("nb", "L1", "2", "b")})}};
    const std::string net = emit(c);
    // Sense from the port node INTO the internal node: I(Vsense) = current entering pin 1.
    CHECK_THAT(net, ContainsSubstring("Vsense_L1 a a__L1 DC 0"));
    CHECK_THAT(net, ContainsSubstring("V=ddt(0.001*atan(I(Vsense_L1)))"));
    // LTspice branch uses the native attribute with x as the branch current.
    CHECK_THAT(emit(c, CIAS::CircuitSimulator::Ltspice), ContainsSubstring("LL1 a b Flux=0.001*atan(x)"));
}

TEST_CASE("sibling current reference i(NAME) maps to I(VNAME)", "[cias]") {
    json src = {{"behavioral", {{"nature", "source"}, {"quantity", "voltage"},
                                {"across", json::array({"p", "n"})}, {"value", 0.0}}},
                {"inputs", {{"designRequirements", json::object()}}}};
    json beh = {{"behavioral", {{"nature", "flux"}, {"expression", "0.002*i(SNS)+0.001*i"}}},
                {"inputs", {{"designRequirements", json::object()}}}};
    json c = {{"name", "cpl"},
              {"ports", json::array({{{"name", "a"}}, {{"name", "b"}}, {{"name", "x"}}})},
              {"components", json::array({{{"name", "SNS"}, {"data", src}},
                                          {{"name", "L1"}, {"data", beh}}})},
              {"connections", json::array({pin_port_net("na", "L1", "1", "a"),
                                           pin_port_net("nb", "L1", "2", "b"),
                                           pin_port_net("nx", "SNS", "p", "x"),
                                           {{"name", "nsg"}, {"endpoints", json::array({
                                               {{"component", "SNS"}, {"pin", "n"}},
                                               {{"component", "L1"}, {"pin", "2"}}})}}})}};
    const std::string net = emit(c);
    CHECK_THAT(net, ContainsSubstring("VSNS x"));                      // 0 V ammeter source
    CHECK_THAT(net, ContainsSubstring("0.002*I(VSNS)+0.001*I(Vsense_L1)"));
}

TEST_CASE("switch nature emits S card with dialect-specific SW model", "[cias]") {
    json sw = {{"behavioral", {{"nature", "switch"},
                               {"across", json::array({"p", "n"})},
                               {"control", json::array({"cp", "cn"})},
                               {"ron", 0.05}, {"roff", 1e9}, {"vOn", 2.5}, {"vOff", 1.5}}},
               {"inputs", {{"designRequirements", json::object()}}}};
    json c = {{"name", "swb"},
              {"ports", json::array({{{"name", "p"}}, {{"name", "n"}}, {{"name", "cp"}}, {{"name", "cn"}}})},
              {"components", json::array({{{"name", "S1"}, {"data", sw}}})},
              {"connections", json::array({pin_port_net("n1", "S1", "p", "p"),
                                           pin_port_net("n2", "S1", "n", "n"),
                                           pin_port_net("n3", "S1", "cp", "cp"),
                                           pin_port_net("n4", "S1", "cn", "cn")})}};
    CHECK_THAT(emit(c), ContainsSubstring(".model SWM_S1 SW(Ron=0.05 Roff=1000000000 Von=2.5 Voff=1.5)"));
    CHECK_THAT(emit(c, CIAS::CircuitSimulator::Ltspice),
               ContainsSubstring(".model SWM_S1 SW(Ron=0.05 Roff=1000000000 Vt=2 Vh=0.5)"));
    json bad = c;
    bad["components"][0]["data"]["behavioral"]["vOn"] = 1.0;  // vOn <= vOff
    CHECK_THROWS_WITH(emit(bad), ContainsSubstring("vOn <= vOff"));
}

TEST_CASE("comparator without behavioral throws — no fabricated ideal part", "[cias]") {
    json cmp = {{"analog", {{"comparator", {{"manufacturerInfo", {{"name", "TI"}}}}}}},
                {"inputs", {{"designRequirements", json::object()}}}};
    json c = {{"name", "cb"},
              {"ports", json::array({{{"name", "o"}}})},
              {"components", json::array({{{"name", "U1"}, {"data", cmp}}})},
              {"connections", json::array({pin_port_net("no", "U1", "out", "o")})}};
    CHECK_THROWS_WITH(emit(c), ContainsSubstring("no behavioral block"));
}

TEST_CASE("comparator behavioral emits SW model; strict required fields", "[cias]") {
    auto circ = [](json behavioral) {
        json cmp = {{"analog", {{"comparator", {{"behavioral", behavioral}}}}},
                    {"inputs", {{"designRequirements", json::object()}}}};
        return json{{"name", "cb"},
                    {"ports", json::array({{{"name", "ip"}}, {{"name", "im"}}, {{"name", "o"}}})},
                    {"components", json::array({{{"name", "U1"}, {"data", cmp}}})},
                    {"connections", json::array({pin_port_net("n1", "U1", "inPlus", "ip"),
                                                 pin_port_net("n2", "U1", "inMinus", "im"),
                                                 pin_port_net("n3", "U1", "out", "o")})}};
    };
    const std::string net = emit(circ({{"outputHigh", 5.0}, {"outputLow", 0.0}, {"hysteresis", 0.01}}));
    CHECK_THAT(net, ContainsSubstring(".model CMP_U1 SW(Vt=0 Vh=0.01 Ron=1 Roff=1e9)"));
    CHECK_THROWS_WITH(emit(circ({{"outputLow", 0.0}})), ContainsSubstring("outputHigh"));
    CHECK_THROWS_WITH(emit(circ({{"outputHigh", 5.0}, {"outputLow", 0.0}, {"hysteresis", -0.1}})),
                      ContainsSubstring("negative hysteresis"));
}

TEST_CASE("integrator clamps are optional and one-sided", "[cias]") {
    auto circ = [](json behavioral) {
        json it = {{"analog", {{"integrator", {{"behavioral", behavioral}}}}},
                   {"inputs", {{"designRequirements", json::object()}}}};
        return json{{"name", "ib"},
                    {"ports", json::array({{{"name", "i"}}, {{"name", "o"}}})},
                    {"components", json::array({{{"name", "G1"}, {"data", it}}})},
                    {"connections", json::array({pin_port_net("n1", "G1", "in", "i"),
                                                 pin_port_net("n2", "G1", "out", "o")})}};
    };
    // Unclamped: plain integrator, output follows the state directly.
    const std::string plain = emit(circ({{"gain", 100.0}}));
    CHECK_THAT(plain, ContainsSubstring("BG1 o 0 V=V(G1__raw)"));
    // Upper clamp only: anti-windup references only the high side.
    const std::string hi = emit(circ({{"gain", 100.0}, {"outputHigh", 1.0}}));
    CHECK_THAT(hi, ContainsSubstring("V(G1__raw)>(1)"));
    CHECK(hi.find("<(") == std::string::npos);
    // Missing gain throws.
    CHECK_THROWS_WITH(emit(circ(json::object())), ContainsSubstring("gain"));
}

TEST_CASE("magnetic: sec-sec coupling is k_i*k_j and corrupt leakage throws", "[cias]") {
    auto circ = [](json dr) {
        json mag = {{"magnetic", json::object()}, {"inputs", {{"designRequirements", dr}}}};
        return json{{"name", "xfmr"},
                    {"ports", json::array({{{"name", "ps"}}, {{"name", "pe"}},
                                           {{"name", "s1s"}}, {{"name", "s1e"}},
                                           {{"name", "s2s"}}, {{"name", "s2e"}}})},
                    {"components", json::array({{{"name", "T1"}, {"data", mag}}})},
                    {"connections", json::array({pin_port_net("n1", "T1", "primary_start", "ps"),
                                                 pin_port_net("n2", "T1", "primary_end", "pe"),
                                                 pin_port_net("n3", "T1", "secondary1_start", "s1s"),
                                                 pin_port_net("n4", "T1", "secondary1_end", "s1e"),
                                                 pin_port_net("n5", "T1", "secondary2_start", "s2s"),
                                                 pin_port_net("n6", "T1", "secondary2_end", "s2e")})}};
    };
    json dr = {{"magnetizingInductance", {{"nominal", 1e-3}}},
               {"turnsRatios", json::array({2.0, 2.0})},
               // Lleak = Lp*(1-k^2) with k=0.8 -> Lleak = 0.36e-3
               {"leakageInductance", json::array({0.00036, 0.00036})}};
    const std::string net = emit(circ(dr));
    CHECK_THAT(net, ContainsSubstring("KT1_0_1 LT1_pri LT1_sec1 0.8"));
    // sec-sec: 0.8*0.8 = 0.64 (was min(k_i,k_j)=0.8 before the fix)
    CHECK_THAT(net, ContainsSubstring("KT1_1_2 LT1_sec1 LT1_sec2 0.64"));

    json bad = dr; bad["leakageInductance"] = json::array({0.002});  // > Lp
    CHECK_THROWS_WITH(emit(circ(bad)), ContainsSubstring("leakage inductance >= magnetizing"));
}

TEST_CASE("unknown pin name in a connection throws instead of floating silently", "[cias]") {
    json c = rc_circuit();
    c["connections"][0]["endpoints"][0]["pin"] = "gat";  // typo
    CHECK_THROWS_WITH(emit(c), ContainsSubstring("unknown pin 'gat'"));
}

TEST_CASE("a net exposed at two ports throws", "[cias]") {
    json c = rc_circuit();
    c["connections"][0]["endpoints"].push_back({{"port", "gnd"}});
    CHECK_THROWS_WITH(emit(c), ContainsSubstring("exposed at two ports"));
}

TEST_CASE("structural validator flags double-port nets", "[cias]") {
    json c = rc_circuit();
    c["connections"][0]["endpoints"].push_back({{"port", "gnd"}});
    auto problems = CIAS::validate_cias_structure(CIAS::CiasCircuit::from_json(c));
    bool found = false;
    for (const auto& p : problems) found |= p.find("exposed at 2 ports") != std::string::npos;
    CHECK(found);
}
