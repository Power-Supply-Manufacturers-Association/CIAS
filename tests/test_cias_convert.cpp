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

//   * timeBase atoms (TDAS oscillator/timer/latch, PEAS-RFC 0001 §7) and the AAS sampleHold
//     emit one canonical template each; missing behavioral fields throw
//   * `time` passes through controlled-nature expressions unmangled (both dialects)

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
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

// A spiceModel is OPTIONAL for the two devices that have an ideal realization, and required
// for the two that do not. The test below used to assert that ANY semiconductor without one
// throws, which was the contract before model-less emission was introduced: rendering circuits
// that need not carry vendor models is the point of CIAS, so a diode now emits the ideal D
// template built from its electrical block. Both halves of the current rule are pinned here —
// the permissive one and the strict one — because a test that only checked the throw would go
// on passing if model-less emission silently stopped working.
TEST_CASE("a diode needs no spiceModel; a bjt does", "[cias]") {
    auto circ = [](const char* devKey, const char* name, json pins) {
        json dev = {{"semiconductor", {{devKey, json::object()}}},
                    {"inputs", {{"designRequirements", json::object()}}}};
        json ports = json::array(), conns = json::array();
        for (auto& p : pins) {
            ports.push_back({{"name", p[1].get<std::string>()}});
            conns.push_back(pin_port_net("n" + p[1].get<std::string>(), name,
                                         p[0].get<std::string>(), p[1].get<std::string>()));
        }
        return json{{"name", "c"}, {"ports", ports},
                    {"components", json::array({{{"name", name}, {"data", dev}}})},
                    {"connections", conns}};
    };
    // diode: no spiceModel is fine — an ideal D card and its .model are emitted.
    const std::string net = emit(circ("diode", "D1",
                                      json::array({{"anode", "a"}, {"cathode", "k"}})));
    CHECK_THAT(net, ContainsSubstring("DD1 a k "));
    CHECK_THAT(net, ContainsSubstring(".model"));

    // bjt: no ideal realization exists, so it must still refuse rather than invent one.
    CHECK_THROWS_WITH(emit(circ("bjt", "Q1",
                                json::array({{"collector", "c"}, {"base", "b"},
                                             {"emitter", "e"}}))),
                      ContainsSubstring("must carry a spiceModel"));
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
    // The magnetic path names mutual couplings "<name>_<ij>" with the winding indices
    // CONCATENATED — KT1_01, not KT1_0_1. That spelling is deliberate and load-bearing:
    // Kirchhoff's decks and its MKF-equivalence tests are written against it. (The
    // coupledInductors path uses "<name>_<i>_<j>" instead; the two conventions disagree,
    // which is filed separately — it is not this test's business to reconcile them.)
    CHECK_THAT(net, ContainsSubstring("KT1_01 LT1_pri LT1_sec1 0.8"));
    // sec-sec: 0.8*0.8 = 0.64 (was min(k_i,k_j)=0.8 before the fix)
    CHECK_THAT(net, ContainsSubstring("KT1_12 LT1_sec1 LT1_sec2 0.64"));

    json bad = dr; bad["leakageInductance"] = json::array({0.002});  // > Lp
    CHECK_THROWS_WITH(emit(circ(bad)), ContainsSubstring("leakage inductance >= magnetizing"));
}

// ─────────────────────────────────────────────────────────────────────────────
// behavioral coupledInductors — N-winding inductance matrix -> L + K cards
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Brick with one coupledInductors component 'PF' whose winding pins are exposed at
// ports a1/b1, a2/b2, ... (winding<i>_start -> a<i>, winding<i>_end -> b<i>).
json coupled_circuit(json matrix, json seriesResistance = nullptr) {
    json beh = {{"nature", "coupledInductors"}, {"inductanceMatrix", matrix}};
    if (!seriesResistance.is_null()) beh["seriesResistance"] = seriesResistance;
    json comp = {{"behavioral", beh},
                 {"inputs", {{"designRequirements", json::object()}}}};
    const size_t n = matrix.size();
    json ports = json::array();
    json conns = json::array();
    for (size_t i = 1; i <= n; ++i) {
        const std::string idx = std::to_string(i);
        ports.push_back({{"name", "a" + idx}});
        ports.push_back({{"name", "b" + idx}});
        conns.push_back(pin_port_net("na" + idx, "PF", "winding" + idx + "_start", "a" + idx));
        conns.push_back(pin_port_net("nb" + idx, "PF", "winding" + idx + "_end", "b" + idx));
    }
    return {{"name", "pinfield"},
            {"ports", ports},
            {"components", json::array({{{"name", "PF"}, {"data", comp}}})},
            {"connections", conns}};
}

} // namespace

TEST_CASE("coupledInductors 2-winding: L cards + hand-computed k", "[cias][coupled]") {
    // k12 = 20n / sqrt(100n * 50n) = 20/sqrt(5000) = 0.2828427125 (10 significant digits).
    const json m = {{100e-9, 20e-9}, {20e-9, 50e-9}};
    const std::string net = emit(coupled_circuit(m));
    CHECK_THAT(net, ContainsSubstring("LPF_1 a1 b1 1e-07"));
    CHECK_THAT(net, ContainsSubstring("LPF_2 a2 b2 5e-08"));
    CHECK_THAT(net, ContainsSubstring("KPF_1_2 LPF_1 LPF_2 0.2828427125"));
    // LTspice: identical cards for |k| < 1.
    const std::string lt = emit(coupled_circuit(m), CIAS::CircuitSimulator::Ltspice);
    CHECK_THAT(lt, ContainsSubstring("KPF_1_2 LPF_1 LPF_2 0.2828427125"));
}

TEST_CASE("coupledInductors 3-winding: seriesResistance nodes, k values, M=0 pair skipped",
          "[cias][coupled]") {
    // k12 = 30n/sqrt(100n*400n) = 30/200 = 0.15; k23 = 60n/sqrt(400n*900n) = 60/600 = 0.1;
    // M13 = 0 -> no K card for the 1-3 pair.
    const json m = {{100e-9, 30e-9, 0.0}, {30e-9, 400e-9, 60e-9}, {0.0, 60e-9, 900e-9}};
    const std::string net = emit(coupled_circuit(m, {0.003, 0.005, 0.0}));
    CHECK_THAT(net, ContainsSubstring("RPF_1 a1 PF__w1 0.003"));
    CHECK_THAT(net, ContainsSubstring("LPF_1 PF__w1 b1 1e-07"));
    CHECK_THAT(net, ContainsSubstring("RPF_2 a2 PF__w2 0.005"));
    CHECK_THAT(net, ContainsSubstring("LPF_2 PF__w2 b2 4e-07"));
    // 0 Ohm entry -> the documented negligible-value realization (same as the 0-ohm resistor).
    CHECK_THAT(net, ContainsSubstring("RPF_3 a3 PF__w3 1e-12"));
    CHECK_THAT(net, ContainsSubstring("LPF_3 PF__w3 b3 9e-07"));
    CHECK_THAT(net, ContainsSubstring("KPF_1_2 LPF_1 LPF_2 0.15"));
    CHECK_THAT(net, ContainsSubstring("KPF_2_3 LPF_2 LPF_3 0.1"));
    CHECK(net.find("KPF_1_3") == std::string::npos);
}

TEST_CASE("coupledInductors k == 1: ngspice caps the EMITTED value, LTspice does not",
          "[cias][coupled]") {
    // M12 = sqrt(L11*L22) exactly -> k = 1: valid data, but ngspice cannot solve k == 1.
    const json m = {{100e-9, 100e-9}, {100e-9, 100e-9}};
    CHECK_THAT(emit(coupled_circuit(m)),
               ContainsSubstring("KPF_1_2 LPF_1 LPF_2 0.999999"));
    CHECK_THAT(emit(coupled_circuit(m), CIAS::CircuitSimulator::Ltspice),
               ContainsSubstring("KPF_1_2 LPF_1 LPF_2 1"));
}

TEST_CASE("coupledInductors strict validation throws std::invalid_argument", "[cias][coupled]") {
    // Asymmetric matrix (beyond 1e-9 relative).
    CHECK_THROWS_AS(emit(coupled_circuit({{100e-9, 20e-9}, {21e-9, 50e-9}})),
                    std::invalid_argument);
    CHECK_THROWS_WITH(emit(coupled_circuit({{100e-9, 20e-9}, {21e-9, 50e-9}})),
                      ContainsSubstring("asymmetric"));
    // |k| > 1: M12 = 80n > sqrt(100n*50n) = 70.7n.
    CHECK_THROWS_AS(emit(coupled_circuit({{100e-9, 80e-9}, {80e-9, 50e-9}})),
                    std::invalid_argument);
    CHECK_THROWS_WITH(emit(coupled_circuit({{100e-9, 80e-9}, {80e-9, 50e-9}})),
                      ContainsSubstring("unphysical coupling"));
    // Negative / zero diagonal.
    CHECK_THROWS_AS(emit(coupled_circuit({{100e-9, 20e-9}, {20e-9, -50e-9}})),
                    std::invalid_argument);
    CHECK_THROWS_WITH(emit(coupled_circuit({{100e-9, 20e-9}, {20e-9, -50e-9}})),
                      ContainsSubstring("non-positive self inductance"));
    CHECK_THROWS_WITH(emit(coupled_circuit({{0.0}})),
                      ContainsSubstring("non-positive self inductance"));
    // Non-square matrix.
    CHECK_THROWS_AS(emit(coupled_circuit({{100e-9, 20e-9}, {20e-9}})),
                    std::invalid_argument);
    CHECK_THROWS_WITH(emit(coupled_circuit({{100e-9, 20e-9}, {20e-9}})),
                      ContainsSubstring("not square"));
    // seriesResistance length mismatch / negative value.
    CHECK_THROWS_AS(emit(coupled_circuit({{100e-9, 20e-9}, {20e-9, 50e-9}}, {0.003})),
                    std::invalid_argument);
    CHECK_THROWS_WITH(emit(coupled_circuit({{100e-9, 20e-9}, {20e-9, 50e-9}}, {0.003})),
                      ContainsSubstring("seriesResistance has 1 entries"));
    CHECK_THROWS_WITH(emit(coupled_circuit({{100e-9, 20e-9}, {20e-9, 50e-9}}, {0.003, -0.1})),
                      ContainsSubstring("negative seriesResistance"));
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

// ─────────────────────────────────────────────────────────────────────────────
// TDAS time-base atoms + AAS sampleHold (PEAS-RFC 0001 §7)
// ─────────────────────────────────────────────────────────────────────────────

namespace {

json time_base_atom(const std::string& family, json behavioral) {
    return {{"timeBase", {{family, {{"behavioral", behavioral}}}}},
            {"inputs", {{"designRequirements", json::object()}}}};
}

// Oscillator brick: pins outPlus/outMinus at ports op/om (+ ctrlPlus/ctrlMinus at cp/cm).
json osc_circuit(json behavioral, bool vco = false) {
    json conns = json::array({pin_port_net("n1", "OSC", "outPlus", "op"),
                              pin_port_net("n2", "OSC", "outMinus", "om")});
    json ports = json::array({{{"name", "op"}}, {{"name", "om"}}});
    if (vco) {
        ports.push_back({{"name", "cp"}});
        ports.push_back({{"name", "cm"}});
        conns.push_back(pin_port_net("n3", "OSC", "ctrlPlus", "cp"));
        conns.push_back(pin_port_net("n4", "OSC", "ctrlMinus", "cm"));
    }
    return {{"name", "oscb"},
            {"ports", ports},
            {"components", json::array({{{"name", "OSC"},
                                         {"data", time_base_atom("oscillator", behavioral)}}})},
            {"connections", conns}};
}

json latch_circuit(json behavioral) {
    return {{"name", "latchb"},
            {"ports", json::array({{{"name", "sp"}}, {{"name", "sm"}}, {{"name", "rp"}},
                                   {{"name", "rm"}}, {{"name", "op"}}, {{"name", "om"}}})},
            {"components", json::array({{{"name", "L1"},
                                         {"data", time_base_atom("latch", behavioral)}}})},
            {"connections", json::array({pin_port_net("n1", "L1", "setPlus", "sp"),
                                         pin_port_net("n2", "L1", "setMinus", "sm"),
                                         pin_port_net("n3", "L1", "resetPlus", "rp"),
                                         pin_port_net("n4", "L1", "resetMinus", "rm"),
                                         pin_port_net("n5", "L1", "outPlus", "op"),
                                         pin_port_net("n6", "L1", "outMinus", "om")})}};
}

json timer_circuit(json behavioral, bool monostable) {
    json conns = json::array({pin_port_net("n1", "M1", "outPlus", "op"),
                              pin_port_net("n2", "M1", "outMinus", "om")});
    json ports = json::array({{{"name", "op"}}, {{"name", "om"}}});
    if (monostable) {
        ports.push_back({{"name", "tp"}});
        ports.push_back({{"name", "tm"}});
        conns.push_back(pin_port_net("n3", "M1", "trgPlus", "tp"));
        conns.push_back(pin_port_net("n4", "M1", "trgMinus", "tm"));
    }
    return {{"name", "timerb"},
            {"ports", ports},
            {"components", json::array({{{"name", "M1"},
                                         {"data", time_base_atom("timer", behavioral)}}})},
            {"connections", conns}};
}

json samplehold_circuit(json behavioral) {
    json sh = {{"analog", {{"sampleHold", {{"behavioral", behavioral}}}}},
               {"inputs", {{"designRequirements", json::object()}}}};
    return {{"name", "shb"},
            {"ports", json::array({{{"name", "i"}}, {{"name", "t"}}, {{"name", "o"}}})},
            {"components", json::array({{{"name", "U1"}, {"data", sh}}})},
            {"connections", json::array({pin_port_net("n1", "U1", "in", "i"),
                                         pin_port_net("n2", "U1", "trg", "t"),
                                         pin_port_net("n3", "U1", "out", "o")})}};
}

} // namespace

TEST_CASE("oscillator fixed sawtooth/triangle/square emit PULSE per span convention", "[cias][tbas]") {
    // sawtooth 100 kHz, amplitude 1, offset 0: rise = T - eps, fall = eps (eps = T/1000).
    const std::string saw = emit(osc_circuit(
        {{"shape", "sawtooth"}, {"frequency", 100000.0}, {"amplitude", 1.0}, {"offset", 0.0}}));
    CHECK_THAT(saw, ContainsSubstring("VOSC op om PULSE(0 1 0 9.99e-06 1e-08 0 1e-05)"));
    // LTspice PULSE card is identical.
    const std::string sawLt = emit(osc_circuit(
        {{"shape", "sawtooth"}, {"frequency", 100000.0}, {"amplitude", 1.0}, {"offset", 0.0}}),
        CIAS::CircuitSimulator::Ltspice);
    CHECK_THAT(sawLt, ContainsSubstring("VOSC op om PULSE(0 1 0 9.99e-06 1e-08 0 1e-05)"));
    // triangle 100 kHz, amplitude 2, offset 1 (span 1..3), phase pi/2 -> td = 2.5e-06:
    // rise = fall = T/2.
    const std::string tri = emit(osc_circuit(
        {{"shape", "triangle"}, {"frequency", 100000.0}, {"amplitude", 2.0}, {"offset", 1.0},
         {"phase", 1.5707963267948966}}));
    CHECK_THAT(tri, ContainsSubstring("VOSC op om PULSE(1 3 2.5e-06 5e-06 5e-06 0 1e-05)"));
    // square 100 kHz, duty 0.25: width = duty*T, edges = eps.
    const std::string sq = emit(osc_circuit(
        {{"shape", "square"}, {"frequency", 100000.0}, {"amplitude", 1.0}, {"offset", 0.0},
         {"dutyCycle", 0.25}}));
    CHECK_THAT(sq, ContainsSubstring("VOSC op om PULSE(0 1 0 1e-08 1e-08 2.5e-06 1e-05)"));
}

TEST_CASE("oscillator fixed sine emits SIN (ngspice) / SINE (LTspice), offset±amplitude", "[cias][tbas]") {
    json b = {{"shape", "sine"}, {"frequency", 100000.0}, {"amplitude", 0.5}, {"offset", 0.5}};
    CHECK_THAT(emit(osc_circuit(b)), ContainsSubstring("VOSC op om SIN(0.5 0.5 100000 0)"));
    CHECK_THAT(emit(osc_circuit(b), CIAS::CircuitSimulator::Ltspice),
               ContainsSubstring("VOSC op om SINE(0.5 0.5 100000 0)"));
}

TEST_CASE("oscillator strict fields: square needs dutyCycle, others must not carry it", "[cias][tbas]") {
    CHECK_THROWS_WITH(emit(osc_circuit(
        {{"shape", "square"}, {"frequency", 1e5}, {"amplitude", 1.0}, {"offset", 0.0}})),
        ContainsSubstring("dutyCycle"));
    CHECK_THROWS_WITH(emit(osc_circuit(
        {{"shape", "sawtooth"}, {"frequency", 1e5}, {"amplitude", 1.0}, {"offset", 0.0},
         {"dutyCycle", 0.5}})),
        ContainsSubstring("shape is not 'square'"));
    CHECK_THROWS_WITH(emit(osc_circuit(
        {{"shape", "sawtooth"}, {"frequency", 1e5}, {"amplitude", 1.0}})),
        ContainsSubstring("offset"));
}

TEST_CASE("timeBase datasheet-only part throws — no fabricated ideal block", "[cias][tbas]") {
    json part = {{"timeBase", {{"oscillator", {{"manufacturerInfo", {{"name", "SiTime"}}}}}}},
                 {"inputs", {{"designRequirements", json::object()}}}};
    json c = {{"name", "oscb"},
              {"ports", json::array({{{"name", "op"}}, {{"name", "om"}}})},
              {"components", json::array({{{"name", "OSC"}, {"data", part}}})},
              {"connections", json::array({pin_port_net("n1", "OSC", "outPlus", "op"),
                                           pin_port_net("n2", "OSC", "outMinus", "om")})}};
    CHECK_THROWS_WITH(emit(c), ContainsSubstring("no behavioral block"));
}

TEST_CASE("VCO emits the phase-accumulator template: 0-clamp and floor() wrap", "[cias][tbas]") {
    json b = {{"shape", "sawtooth"}, {"frequency", 100000.0}, {"amplitude", 1.0}, {"offset", 0.0},
              {"frequencyControl", {{"gain", 50000.0}}}};
    const std::string net = emit(osc_circuit(b, true));
    // Clamped instantaneous frequency charging the 1 F phase capacitor (V(ph) = cycles).
    CHECK_THAT(net, ContainsSubstring("BOSC_f 0 OSC__ph I=max(0,(100000)+(50000)*V(cp,cm))"));
    CHECK_THAT(net, ContainsSubstring("COSC_ph OSC__ph 0 1 IC=0"));
    // Shaping B-source wraps the phase with floor().
    CHECK_THAT(net, ContainsSubstring("BOSC op om V=(0)+(1)*(V(OSC__ph)-floor(V(OSC__ph)))"));
    // Sine VCO shapes with sin(2*pi*phase); LTspice keeps the same template.
    json bs = b; bs["shape"] = "sine";
    CHECK_THAT(emit(osc_circuit(bs, true)), ContainsSubstring("*sin(2*pi*(V(OSC__ph)-floor(V(OSC__ph))))"));
    const std::string lt = emit(osc_circuit(b, true), CIAS::CircuitSimulator::Ltspice);
    CHECK_THAT(lt, ContainsSubstring("I=max(0,(100000)+(50000)*V(cp,cm))"));
    // Missing gain throws.
    json bad = b; bad["frequencyControl"] = json::object();
    CHECK_THROWS_WITH(emit(osc_circuit(bad, true)), ContainsSubstring("gain"));
}

TEST_CASE("latch dominance decides the ternary nesting order", "[cias][tbas]") {
    json b = {{"setThreshold", 2.0}, {"resetThreshold", 1.0},
              {"outputHigh", 10.0}, {"outputLow", 0.0}, {"dominance", "reset"}};
    const std::string rd = emit(latch_circuit(b));
    // Reset-dominant: the RESET comparison is the OUTER ternary.
    CHECK_THAT(rd, ContainsSubstring(
        "BL1_st L1__d 0 V=(V(rp,rm)>(1))?(0):((V(sp,sm)>(2))?(1):(V(L1__q)))"));
    // 1 ns RC on the state node + linear 0/1 -> outputLow/outputHigh output stage.
    CHECK_THAT(rd, ContainsSubstring("RL1_st L1__d L1__q 1"));
    CHECK_THAT(rd, ContainsSubstring("CL1_st L1__q 0 1n"));
    CHECK_THAT(rd, ContainsSubstring("BL1 op om V=(0)+((10)-(0))*V(L1__q)"));
    json bs = b; bs["dominance"] = "set";
    CHECK_THAT(emit(latch_circuit(bs)), ContainsSubstring(
        "BL1_st L1__d 0 V=(V(sp,sm)>(2))?(1):((V(rp,rm)>(1))?(0):(V(L1__q)))"));
    // LTspice uses the if() idiom with the same ordering.
    CHECK_THAT(emit(latch_circuit(b), CIAS::CircuitSimulator::Ltspice), ContainsSubstring(
        "BL1_st L1__d 0 V=if(V(rp,rm)>(1),0,if(V(sp,sm)>(2),1,V(L1__q)))"));
    json bad = b; bad.erase("dominance");
    CHECK_THROWS_WITH(emit(latch_circuit(bad)), ContainsSubstring("dominance"));
}

TEST_CASE("timer astable emits PULSE between outputLow/outputHigh", "[cias][tbas]") {
    json b = {{"mode", "astable"}, {"outputHigh", 10.0}, {"outputLow", 0.0},
              {"period", 2e-6}, {"dutyCycle", 0.5}};
    CHECK_THAT(emit(timer_circuit(b, false)),
               ContainsSubstring("VM1 op om PULSE(0 10 0 2e-09 2e-09 1e-06 2e-06)"));
    json bad = b; bad.erase("period");
    CHECK_THROWS_WITH(emit(timer_circuit(bad, false)), ContainsSubstring("period"));
}

TEST_CASE("timer monostable: retriggerable decides edge handling and ramp reset", "[cias][tbas]") {
    json b = {{"mode", "monostable"}, {"outputHigh", 10.0}, {"outputLow", 0.0},
              {"threshold", 2.5}, {"polarity", "risingEdge"}, {"onTime", 5e-6},
              {"retriggerable", false}};
    const std::string nr = emit(timer_circuit(b, true));
    // Trigger level, edge-detect RC, time ramp (1 V/s), discharge switch, output stage.
    CHECK_THAT(nr, ContainsSubstring("BM1_lvl M1__lvl 0 V=(V(tp,tm)>(2.5))?(1):(0)"));
    CHECK_THAT(nr, ContainsSubstring("RM1_ed M1__lvl M1__lvld 10"));
    CHECK_THAT(nr, ContainsSubstring("BM1_chg 0 M1__tr I=1e-9"));
    CHECK_THAT(nr, ContainsSubstring("CM1_tr M1__tr 0 1n"));
    CHECK_THAT(nr, ContainsSubstring("SM1_rst M1__tr 0 M1__rst 0 TMR_M1"));
    CHECK_THAT(nr, ContainsSubstring(".model TMR_M1 SW(Vt=0.5 Vh=0.2 Ron=1 Roff=1e9)"));
    CHECK_THAT(nr, ContainsSubstring("BM1 op om V=(0)+((10)-(0))*V(M1__q)"));
    // Non-retriggerable: edges only reach the IDLE branch; ramp discharges only while idle.
    CHECK_THAT(nr, ContainsSubstring(
        "BM1_st M1__d 0 V=(V(M1__q)>0.5)?((V(M1__tr)>=(5e-06))?(0):(1)):"
        "((V(M1__lvl)-V(M1__lvld)>0.5)?(1):(0))"));
    CHECK_THAT(nr, ContainsSubstring("BM1_rst M1__rst 0 V=(V(M1__q)<0.5)?(1):(0)"));
    // Retriggerable: an edge always (re)sets the state AND discharges the ramp.
    json br = b; br["retriggerable"] = true;
    const std::string rt = emit(timer_circuit(br, true));
    CHECK_THAT(rt, ContainsSubstring(
        "BM1_st M1__d 0 V=(V(M1__lvl)-V(M1__lvld)>0.5)?(1):"
        "((V(M1__q)>0.5)?((V(M1__tr)>=(5e-06))?(0):(1)):(0))"));
    CHECK_THAT(rt, ContainsSubstring(
        "BM1_rst M1__rst 0 V=(V(M1__lvl)-V(M1__lvld)>0.5)?(1):((V(M1__q)<0.5)?(1):(0))"));
    // fallingEdge flips the level comparison.
    json bf = b; bf["polarity"] = "fallingEdge";
    CHECK_THAT(emit(timer_circuit(bf, true)),
               ContainsSubstring("BM1_lvl M1__lvl 0 V=(V(tp,tm)<(2.5))?(1):(0)"));
    // Missing retriggerable throws (the two behaviors are a real design decision).
    json bad = b; bad.erase("retriggerable");
    CHECK_THROWS_WITH(emit(timer_circuit(bad, true)), ContainsSubstring("retriggerable"));
}

TEST_CASE("sampleHold emits switch + 1 nF hold cap + E-buffer per mode", "[cias][tbas]") {
    json b = {{"mode", "trackWhileActive"}, {"threshold", 2.5}, {"polarity", "activeHigh"}};
    const std::string trk = emit(samplehold_circuit(b));
    CHECK_THAT(trk, ContainsSubstring("BU1_ctl U1__ctl 0 V=(V(t)>(2.5))?(1):(0)"));
    CHECK_THAT(trk, ContainsSubstring("SU1 i U1__hold U1__ctl 0 SHM_U1"));
    CHECK_THAT(trk, ContainsSubstring("CU1_hold U1__hold 0 1n"));
    CHECK_THAT(trk, ContainsSubstring("EU1 o 0 U1__hold 0 1"));
    CHECK_THAT(trk, ContainsSubstring(".model SHM_U1 SW(Vt=0.5 Vh=0.2 Ron=1 Roff=1e9)"));
    // activeLow flips the trigger comparison; LTspice uses if().
    json bl = b; bl["polarity"] = "activeLow";
    CHECK_THAT(emit(samplehold_circuit(bl)), ContainsSubstring("V=(V(t)<(2.5))?(1):(0)"));
    CHECK_THAT(emit(samplehold_circuit(b), CIAS::CircuitSimulator::Ltspice),
               ContainsSubstring("BU1_ctl U1__ctl 0 V=if(V(t)>(2.5),1,0)"));
    // sampleOnEdge adds the edge-detect RC and gates the switch on the edge pulse.
    json be = b; be["mode"] = "sampleOnEdge";
    const std::string edge = emit(samplehold_circuit(be));
    CHECK_THAT(edge, ContainsSubstring("BU1_lvl U1__lvl 0 V=(V(t)>(2.5))?(1):(0)"));
    CHECK_THAT(edge, ContainsSubstring("RU1_ed U1__lvl U1__lvld 10"));
    CHECK_THAT(edge, ContainsSubstring("BU1_ctl U1__ctl 0 V=(V(U1__lvl)-V(U1__lvld)>0.5)?(1):(0)"));
    // Missing threshold throws.
    json bad = b; bad.erase("threshold");
    CHECK_THROWS_WITH(emit(samplehold_circuit(bad)), ContainsSubstring("threshold"));
}

TEST_CASE("controlled-nature expressions pass `time` through unmangled", "[cias][tbas]") {
    json ctl = {{"behavioral", {{"nature", "controlled"},
                                {"output", {{"quantity", "voltage"},
                                            {"across", json::array({"p", "n"})},
                                            {"expression", "0.5+0.4*sin(2*pi*50*time)"}}}}},
                {"inputs", {{"designRequirements", json::object()}}}};
    json c = {{"name", "tsrc"},
              {"ports", json::array({{{"name", "p"}}, {{"name", "n"}}})},
              {"components", json::array({{{"name", "B1"}, {"data", ctl}}})},
              {"connections", json::array({pin_port_net("n1", "B1", "p", "p"),
                                           pin_port_net("n2", "B1", "n", "n")})}};
    CHECK_THAT(emit(c), ContainsSubstring("BB1 p n V=0.5+0.4*sin(2*pi*50*time)"));
    CHECK_THAT(emit(c, CIAS::CircuitSimulator::Ltspice),
               ContainsSubstring("BB1 p n V=0.5+0.4*sin(2*pi*50*time)"));
}

// ngspice smoke test — ngspice was on PATH when this test was written; if it has since
// disappeared the test FAILS loudly (never silently skips) per the house rule.
TEST_CASE("ngspice smoke: PWM duty cycle tracks the control voltage", "[cias][tbas][ngspice]") {
    if (std::system("which ngspice > /dev/null 2>&1") != 0)
        FAIL("ngspice not found on PATH — it was present when this smoke test was added");

    // TDAS sawtooth oscillator (100 kHz, 0..1 V ramp) + AAS comparator: classic voltage-mode
    // PWM. The comparator output is high while V(ctl) > V(ramp), so duty == vctl.
    json osc = time_base_atom("oscillator", {{"shape", "sawtooth"}, {"frequency", 100000.0},
                                             {"amplitude", 1.0}, {"offset", 0.0}});
    json cmp = {{"analog", {{"comparator", {{"behavioral", {{"outputHigh", 1.0},
                                                            {"outputLow", 0.0}}}}}}},
                {"inputs", {{"designRequirements", json::object()}}}};
    json c = {{"name", "pwm"},
              {"ports", json::array()},
              {"components", json::array({{{"name", "OSC"}, {"data", osc}},
                                          {{"name", "CMP"}, {"data", cmp}}})},
              {"connections", json::array({
                  {{"name", "ramp"}, {"endpoints", json::array({
                      {{"component", "OSC"}, {"pin", "outPlus"}},
                      {{"component", "CMP"}, {"pin", "inMinus"}}})}},
                  {{"name", "0"}, {"endpoints", json::array({
                      {{"component", "OSC"}, {"pin", "outMinus"}}})}},
                  {{"name", "ctl"}, {"endpoints", json::array({
                      {{"component", "CMP"}, {"pin", "inPlus"}}})}},
                  {{"name", "pwm"}, {"endpoints", json::array({
                      {{"component", "CMP"}, {"pin", "out"}}})}}})}};
    const std::string cards =
        CIAS::CiasCircuitConverter(CIAS::CircuitSimulator::Ngspice)
            .to_cards(CIAS::CiasCircuit::from_json(c));

    namespace fs = std::filesystem;
    const std::string tag = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path cir = fs::temp_directory_path() / ("cias_pwm_smoke_" + tag + ".cir");
    const fs::path log = fs::temp_directory_path() / ("cias_pwm_smoke_" + tag + ".log");

    auto run_duty = [&](double vctl) -> double {
        {
            std::ofstream f(cir);
            REQUIRE(f.good());
            f << "* CIAS PWM smoke (emitted by CiasCircuitConverter)\n"
              << "Vctl ctl 0 DC " << vctl << "\n"
              << cards
              << ".tran 0.01u 200u 100u\n"
              << ".meas tran davg AVG V(pwm) from=100u to=200u\n"
              << ".end\n";
        }
        const std::string cmd = "ngspice -b " + cir.string() + " > " + log.string() + " 2>&1";
        REQUIRE(std::system(cmd.c_str()) == 0);
        std::ifstream lf(log);
        std::string line;
        while (std::getline(lf, line)) {
            const auto pos = line.find("davg");
            if (pos != std::string::npos) {
                const auto eq = line.find('=', pos);
                REQUIRE(eq != std::string::npos);
                return std::stod(line.substr(eq + 1));
            }
        }
        FAIL("ngspice produced no 'davg' measurement — deck: " + cir.string());
        return 0.0;
    };

    // Average of a 0/1 PWM wave over whole periods IS the duty cycle. Tolerance 5%.
    const double d25 = run_duty(0.25);
    CHECK(std::abs(d25 - 0.25) < 0.05);
    const double d70 = run_duty(0.70);
    CHECK(std::abs(d70 - 0.70) < 0.05);
    fs::remove(cir);
    fs::remove(log);
}

TEST_CASE("structural validator flags double-port nets", "[cias]") {
    json c = rc_circuit();
    c["connections"][0]["endpoints"].push_back({{"port", "gnd"}});
    auto problems = CIAS::validate_cias_structure(CIAS::CiasCircuit::from_json(c));
    bool found = false;
    for (const auto& p : problems) found |= p.find("exposed at 2 ports") != std::string::npos;
    CHECK(found);
}

// ---------------------------------------------------------------------------
// ABT #540 — two drifts between this library and the schemas it mirrors.
// Both were found by review, confirmed by direct test, and are pinned here so
// they cannot come back.
// ---------------------------------------------------------------------------

// Locate PEAS/schemas/peas.json relative to this repo. Returns an empty path if the
// sibling checkout is absent, so the test SKIPS loudly rather than silently passing.
static std::filesystem::path peas_schema_path() {
    for (const char* rel : {"../PEAS/schemas/peas.json", "../../PEAS/schemas/peas.json",
                            "PEAS/schemas/peas.json"}) {
        std::filesystem::path p(rel);
        if (std::filesystem::exists(p)) return p;
    }
    return {};
}

TEST_CASE("validate_cias_structure knows every PEAS discriminator", "[cias][drift]") {
    // The KNOWN list in validate_cias_structure must mirror peas.json's top-level oneOf.
    // It did not: 'thermistor' was absent, so a valid inline PEAS thermistor came back as
    // "0 discriminators (expected exactly 1)". That list decides whether a component is
    // well-formed, and TAS's changed_records_gate now runs it over every brick in the
    // catalogue, so a gap in it is a gate reporting a defect that does not exist.
    const std::filesystem::path ps = peas_schema_path();
    if (ps.empty()) {
        WARN("PEAS/schemas/peas.json not found next to this checkout — drift test skipped");
        return;
    }
    std::ifstream in(ps);
    REQUIRE(in);
    json peas = json::parse(in);

    std::vector<std::string> discriminators;
    for (const auto& branch : peas.at("oneOf"))
        for (const auto& req : branch.at("required"))
            if (req.get<std::string>() != "inputs") discriminators.push_back(req.get<std::string>());
    REQUIRE(discriminators.size() >= 10);

    // Every PEAS discriminator must survive structural validation on a minimal brick.
    for (const std::string& d : discriminators) {
        json brick = json::parse(R"json({"name":"drift","ports":[{"name":"a"},{"name":"b"}],
          "components":[{"name":"X1","data":{"inputs":{"designRequirements":{"name":"x"}}}}],
          "connections":[
            {"name":"n1","endpoints":[{"component":"X1","pin":"1"},{"port":"a"}]},
            {"name":"n2","endpoints":[{"component":"X1","pin":"2"},{"port":"b"}]}]})json");
        brick["components"][0]["data"][d] = json::object();
        const auto problems =
            CIAS::validate_cias_structure(CIAS::CiasCircuit::from_json(brick));
        INFO("PEAS discriminator '" << d << "' rejected by validate_cias_structure");
        CHECK(problems.empty());
    }
}

TEST_CASE("the component data oneOf stays disjoint", "[cias][drift]") {
    // CIAS.json's `data` is oneOf[ PEAS document | URI string ] with NO const
    // discriminator. That is safe only because peas.json pins a top-level object type, so
    // a string can never satisfy the PEAS branch. If PEAS ever drops that pin, every
    // URI-form component in the catalogue starts failing oneOf — silently, at load time,
    // far from the change that caused it. Pin the assumption here.
    const std::filesystem::path ps = peas_schema_path();
    if (ps.empty()) {
        WARN("PEAS/schemas/peas.json not found next to this checkout — disjointness test skipped");
        return;
    }
    std::ifstream in(ps);
    REQUIRE(in);
    json peas = json::parse(in);
    REQUIRE(peas.contains("type"));
    CHECK(peas.at("type").get<std::string>() == "object");
}

TEST_CASE("a brick round-trips its provenance", "[cias][provenance]") {
    // CiasCircuit had no provenance member, so from_json -> to_json SILENTLY dropped the
    // field. 7,554 connector pin-field bricks carry it, and for a derived artifact it is
    // the entire assumption record.
    json brick = json::parse(R"json({"name":"p","ports":[{"name":"a"},{"name":"b"}],
      "components":[{"name":"R1","data":{"resistor":{},
        "inputs":{"designRequirements":{"resistance":{"nominal":50}}}}}],
      "connections":[
        {"name":"n1","endpoints":[{"component":"R1","pin":"1"},{"port":"a"}]},
        {"name":"n2","endpoints":[{"component":"R1","pin":"2"},{"port":"b"}]}],
      "provenance":[{"source":"derived","sourceName":"gen.py","retrievedDate":"2026-08-02",
                     "derivation":"L_ii = (mu0*l/2pi)*(ln(2l/r)-1)"}]})json");

    const json out = CIAS::CiasCircuit::from_json(brick).to_json();
    REQUIRE(out.contains("provenance"));
    CHECK(out["provenance"] == brick["provenance"]);
    CHECK(out["provenance"][0]["derivation"].get<std::string>()
              == "L_ii = (mu0*l/2pi)*(ln(2l/r)-1)");
}

TEST_CASE("a brick without provenance does not grow a null one", "[cias][provenance]") {
    // Emitting an explicit null would turn "no trail" into "the trail is null", and
    // CIAS.json has no default for the field.
    json brick = json::parse(R"json({"name":"q","ports":[{"name":"a"},{"name":"b"}],
      "components":[{"name":"R1","data":{"resistor":{},
        "inputs":{"designRequirements":{"resistance":{"nominal":50}}}}}],
      "connections":[
        {"name":"n1","endpoints":[{"component":"R1","pin":"1"},{"port":"a"}]},
        {"name":"n2","endpoints":[{"component":"R1","pin":"2"},{"port":"b"}]}]})json");
    const json out = CIAS::CiasCircuit::from_json(brick).to_json();
    CHECK_FALSE(out.contains("provenance"));
}

TEST_CASE("an unresolved catalogue URI says so, and names itself", "[cias][uri]") {
    // Legal CIAS, not corruption: it simply has to be resolved before emission. The old
    // message ("has no PEAS data object") read like the brick was broken.
    json brick = json::parse(R"json({"name":"u","ports":[{"name":"a"},{"name":"b"}],
      "components":[{"name":"Qh","data":"TAS/data/mosfets.ndjson?partNumber=C3M0032120K"}],
      "connections":[
        {"name":"n1","endpoints":[{"component":"Qh","pin":"drain"},{"port":"a"}]},
        {"name":"n2","endpoints":[{"component":"Qh","pin":"source"},{"port":"b"}]}]})json");

    // structurally fine — a URI component is a legal placement
    CHECK(CIAS::validate_cias_structure(CIAS::CiasCircuit::from_json(brick)).empty());

    CIAS::CiasCircuitConverter conv(CIAS::CircuitSimulator::Ngspice);
    REQUIRE_THROWS_WITH(conv.to_subckt_json(brick),
                        Catch::Matchers::ContainsSubstring("unresolved catalogue reference") &&
                        Catch::Matchers::ContainsSubstring("C3M0032120K"));
}

TEST_CASE("magnetic: a winding count that would make K-card names ambiguous is refused",
          "[cias]") {
    // "K<name>_<ij>" concatenates the two winding indices with no separator, so it stops
    // being decodable once an index reaches two digits. The danger is not unreadability:
    // two DISTINCT pairs can land on one SPICE element name, and a duplicate K card does
    // not error — the later silently replaces the earlier, leaving windings uncoupled or
    // coupled to the wrong partner. Ten windings (indices 0..9) is the last safe width.
    auto xfmr = [](size_t secondaries) {
        json ports = json::array(), conns = json::array();
        auto add = [&](const std::string& pin, const std::string& port) {
            ports.push_back({{"name", port}});
            conns.push_back(pin_port_net("n" + port, "T1", pin, port));
        };
        add("primary_start", "ps");
        add("primary_end", "pe");
        json ratios = json::array(), leak = json::array();
        for (size_t s = 1; s <= secondaries; ++s) {
            add("secondary" + std::to_string(s) + "_start", "s" + std::to_string(s) + "s");
            add("secondary" + std::to_string(s) + "_end", "s" + std::to_string(s) + "e");
            ratios.push_back(2.0);
            leak.push_back(0.00036);
        }
        json mag = {{"magnetic", json::object()},
                    {"inputs", {{"designRequirements",
                                 {{"magnetizingInductance", {{"nominal", 1e-3}}},
                                  {"turnsRatios", ratios},
                                  {"leakageInductance", leak}}}}}};
        return json{{"name", "x"}, {"ports", ports},
                    {"components", json::array({{{"name", "T1"}, {"data", mag}}})},
                    {"connections", conns}};
    };
    // 10 windings: every index is one digit, so the names stay decodable.
    CHECK_NOTHROW(emit(xfmr(9)));
    // 11 windings: index 10 appears and the format can no longer be trusted.
    CHECK_THROWS_WITH(emit(xfmr(10)), ContainsSubstring("ambiguous beyond ten"));
}

TEST_CASE("magnetic MKF_MODEL path reads outputs.spiceSubcircuit (ABT #947)", "[cias]") {
    // The pre-fitted subcircuit reference lives in the PEAS component's open `outputs` bag --
    // the only home the schemas allow. magnetic.modelOutputs is FORBIDDEN by the closed MAS
    // schema, and anything still writing it must hear so loudly rather than silently fall to
    // the ideal path.
    json mag = {{"magnetic", {{"name", "m"}}},
                {"inputs", {{"designRequirements",
                             {{"magnetizingInductance", {{"nominal", 1e-6}}},
                              {"turnsRatios", json::array()}}}}},
                {"outputs", {{"spiceSubcircuit", {{"reference", "WE_123"}}}}}};
    json c = {{"name", "b"},
              {"ports", json::array({{{"name", "p"}}, {{"name", "n"}}})},
              {"components", json::array({{{"name", "L1"}, {"data", mag}}})},
              {"connections",
               json::array({pin_port_net("np", "L1", "primary_start", "p"),
                            pin_port_net("nn", "L1", "primary_end", "n")})}};
    const std::string net = emit(c);
    CHECK_THAT(net, ContainsSubstring("XL1 p n WE_123"));

    json bad = c;
    bad["components"][0]["data"].erase("outputs");
    bad["components"][0]["data"]["magnetic"]["modelOutputs"] =
        {{"spiceSubcircuit", {{"reference", "WE_123"}}}};
    CHECK_THROWS_WITH(emit(bad), ContainsSubstring("ABT #947"));
}

// ---------------------------------------------------------------------------
// The capacitor's source of truth, in order (ABT #1136).
//
// A catalogue capacitor is the thing being simulated; a designRequirements
// capacitance is what somebody asked for before there was a part. Reading only
// the requirement made a real part unlowerable — a TAS capacitor has no
// `inputs` at all — and emitted an ideal C for one that could be lowered,
// discarding an ESR and ESL the record states.
// ---------------------------------------------------------------------------
namespace {
// `data` is a whole PEAS document: the discriminator key and `inputs` are
// SIBLINGS ({capacitor:{…}, inputs:{…}}), which is the shape capacitor_atom
// above builds and the shape a TAS catalogue record arrives in (minus inputs).
json cap_brick_data(json data) {
    return json{
        {"name", "cb"},
        {"ports", json::array({json{{"name", "a"}}, json{{"name", "b"}}})},
        {"components", json::array({json{{"name", "C1"}, {"data", std::move(data)}}})},
        {"connections", json::array({pin_port_net("na", "C1", "1", "a"),
                                     pin_port_net("nb", "C1", "2", "b")})},
    };
}
json cap_brick(json capacitor) {
    return cap_brick_data(json{{"capacitor", std::move(capacitor)}});
}
json part_with(json datasheet_info) {
    return json{{"manufacturerInfo",
                 json{{"name", "Wurth Elektronik"},
                      {"reference", "885342208014"},
                      {"datasheetInfo", std::move(datasheet_info)}}}};
}
}  // namespace

TEST_CASE("capacitor: modelParams win — Rs and Ls in series, Riso shunting", "[cias]") {
    const std::string net = emit(cap_brick(part_with(json{
        {"electrical", json{{"capacitance", 2.2e-8}, {"esr", 0.787}}},
        {"modelParams", json{{"rs", 0.0616}, {"cs", 2.2e-8}, {"ls", 4.768e-10},
                             {"riso", 4.54e9}}}})));
    CHECK_THAT(net, ContainsSubstring("RC1_esr a "));
    CHECK_THAT(net, ContainsSubstring("0.0616"));
    CHECK_THAT(net, ContainsSubstring("LC1_esl "));
    CHECK_THAT(net, ContainsSubstring("4.768e-10"));
    CHECK_THAT(net, ContainsSubstring("CC1 "));
    CHECK_THAT(net, ContainsSubstring("RC1_iso "));
    // the equivalent circuit's own Cs, not the electrical block's rated value
    CHECK_THAT(net, ContainsSubstring("2.2e-08"));
    // and the electrical ESR is NOT also emitted — one resistor, from modelParams
    CHECK_THAT(net, !ContainsSubstring("0.787"));
}

TEST_CASE("capacitor: no modelParams falls back to the datasheet, ESR included",
          "[cias]") {
    const std::string net = emit(cap_brick(part_with(json{
        {"electrical", json{{"capacitance", 2.2e-8}, {"esr", 0.787}}}})));
    CHECK_THAT(net, ContainsSubstring("RC1_esr a "));
    CHECK_THAT(net, ContainsSubstring("0.787"));
    CHECK_THAT(net, ContainsSubstring("CC1 "));
    CHECK_THAT(net, ContainsSubstring("2.2e-08"));
    // nothing states an ESL, so no inductor is invented
    CHECK_THAT(net, !ContainsSubstring("LC1_esl"));
    CHECK_THAT(net, !ContainsSubstring("RC1_iso"));
}

TEST_CASE("capacitor: a datasheet with no ESR emits the capacitor alone", "[cias]") {
    const std::string net = emit(cap_brick(part_with(json{
        {"electrical", json{{"capacitance", 2.2e-8}}}})));
    CHECK_THAT(net, ContainsSubstring("CC1 a b 2.2e-08"));
    CHECK_THAT(net, !ContainsSubstring("RC1_esr"));
    CHECK_THAT(net, !ContainsSubstring("LC1_esl"));
}

TEST_CASE("capacitor: no part data at all falls back to the requirement", "[cias]") {
    const std::string net = emit(cap_brick_data(capacitor_atom(1e-6)));
    CHECK_THAT(net, ContainsSubstring("CC1 a b 1e-06"));
    CHECK_THAT(net, !ContainsSubstring("RC1_esr"));
}

TEST_CASE("capacitor: neither a datasheet nor a requirement throws", "[cias]") {
    CHECK_THROWS_WITH(
        emit(cap_brick(json{{"manufacturerInfo",
                             json{{"name", "ACME"}, {"reference", "NOTHING"}}}})),
        ContainsSubstring("states no capacitance"));
}

TEST_CASE("capacitor: a part that also carries inputs still prefers the part", "[cias]") {
    // the case the old order got backwards
    json data{{"capacitor", part_with(json{{"electrical",
                  json{{"capacitance", 2.2e-8}, {"esr", 0.787}}}})}};
    data["inputs"]["designRequirements"]["capacitance"] = 1e-6;
    const std::string net = emit(cap_brick_data(data));
    CHECK_THAT(net, ContainsSubstring("2.2e-08"));
    CHECK_THAT(net, !ContainsSubstring("1e-06"));
    CHECK_THAT(net, ContainsSubstring("RC1_esr"));
}

// ---------------------------------------------------------------------------
// The resistor follows the capacitor's order, for the same reason (ABT #1136).
// ---------------------------------------------------------------------------
namespace {
json res_brick_data(json data) {
    return json{
        {"name", "rb"},
        {"ports", json::array({json{{"name", "a"}}, json{{"name", "b"}}})},
        {"components", json::array({json{{"name", "R1"}, {"data", std::move(data)}}})},
        {"connections", json::array({pin_port_net("na", "R1", "1", "a"),
                                     pin_port_net("nb", "R1", "2", "b")})},
    };
}
json res_part(json datasheet_info) {
    return json{{"resistor",
                 json{{"manufacturerInfo",
                       json{{"name", "Yageo"}, {"reference", "RC0402FR-0710KL"},
                            {"datasheetInfo", std::move(datasheet_info)}}}}}};
}
}  // namespace

TEST_CASE("resistor: modelParams win, and carry their temperature coefficients",
          "[cias]") {
    const std::string net = emit(res_brick_data(res_part(json{
        {"electrical", json{{"resistance", 4700.0}}},
        {"modelParams", json{{"r", 10000.0}, {"tcr1", 1e-4}, {"tcr2", 2e-7}}}})));
    CHECK_THAT(net, ContainsSubstring("RR1 a b 10000"));
    CHECK_THAT(net, ContainsSubstring("TC1=0.0001"));
    CHECK_THAT(net, ContainsSubstring("TC2=2e-07"));
    CHECK_THAT(net, !ContainsSubstring("4700"));
}

TEST_CASE("resistor: no modelParams falls back to the datasheet resistance", "[cias]") {
    const std::string net = emit(res_brick_data(res_part(json{
        {"electrical", json{{"resistance", 4700.0}}}})));
    CHECK_THAT(net, ContainsSubstring("RR1 a b 4700"));
    // nothing states a coefficient, so none is invented
    CHECK_THAT(net, !ContainsSubstring("TC1="));
}

TEST_CASE("resistor: no part data at all falls back to the requirement", "[cias]") {
    CHECK_THAT(emit(res_brick_data(resistor_atom(1000.0))),
               ContainsSubstring("RR1 a b 1000"));
}

TEST_CASE("resistor: neither a datasheet nor a requirement throws", "[cias]") {
    CHECK_THROWS_WITH(
        emit(res_brick_data(json{{"resistor",
                                  json{{"manufacturerInfo",
                                        json{{"name", "ACME"}, {"reference", "N"}}}}}})),
        ContainsSubstring("states no resistance"));
}

TEST_CASE("resistor: a part that also carries inputs still prefers the part", "[cias]") {
    json data = res_part(json{{"electrical", json{{"resistance", 4700.0}}}});
    data["inputs"]["designRequirements"]["resistance"] = 1000.0;
    const std::string net = emit(res_brick_data(data));
    CHECK_THAT(net, ContainsSubstring("RR1 a b 4700"));
    CHECK_THAT(net, !ContainsSubstring("RR1 a b 1000"));
}
