#include "CiasCircuitConverter.hpp"
#include <sstream>
#include <map>
#include <vector>
#include <cmath>
#include <algorithm>
#include <set>
#include <stdexcept>
#include <optional>
#include <regex>

namespace CIAS {

namespace {

std::string num(double v) {
    std::ostringstream os;
    os.precision(10);
    os << v;
    return os.str();
}

// Net node name for a connection: a net exposed at a brick port takes the port name, else the
// connection's own (brick-local) name.
std::string net_node_name(const Connection& conn) {
    for (const auto& ep : conn.endpoints)
        if (ep.isPortEndpoint())
            return ep.port;
    return conn.name;
}

// Traverse data[keys...] to a number; throws if absent.
double nominal_at(const json& data, std::initializer_list<const char*> keys, const std::string& what) {
    const json* cur = &data;
    for (const char* k : keys) {
        if (!cur->is_object() || !cur->contains(k))
            throw std::runtime_error("CiasCircuitConverter: missing '" + std::string(k) + "' for " + what);
        cur = &cur->at(k);
    }
    if (cur->is_number()) return cur->get<double>();
    if (cur->is_object() && cur->contains("nominal") && cur->at("nominal").is_number())
        return cur->at("nominal").get<double>();
    throw std::runtime_error("CiasCircuitConverter: non-numeric value for " + what);
}

std::string simulator_name(CircuitSimulator t) {
    switch (t) {
        case CircuitSimulator::Ngspice: return "Ngspice";
        case CircuitSimulator::Ltspice: return "Ltspice";
        case CircuitSimulator::NL5:     return "NL5";
        case CircuitSimulator::Simba:   return "Simba";
        case CircuitSimulator::Plecs:   return "Plecs";
    }
    return "unknown";
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / factory
// ─────────────────────────────────────────────────────────────────────────────

CiasCircuitConverter::CiasCircuitConverter(CircuitSimulator target) : target_(target) {}

std::shared_ptr<CiasCircuitConverter> CiasCircuitConverter::create(CircuitSimulator target) {
    return std::make_shared<CiasCircuitConverter>(target);
}

// ─────────────────────────────────────────────────────────────────────────────
// Private helpers
// ─────────────────────────────────────────────────────────────────────────────

SpiceDialect CiasCircuitConverter::spice_dialect() const {
    return target_ == CircuitSimulator::Ltspice ? SpiceDialect::Ltspice : SpiceDialect::Ngspice;
}

bool CiasCircuitConverter::has_ltspice_declarations(const CiasCircuit& circuit) const {
    for (const auto& c : circuit.components)
        if (c.data.is_object() &&
            c.data.contains("ltspice_declaration") &&
            c.data.at("ltspice_declaration").is_string())
            return true;
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// LTspice passthrough — emits the original ltspice_declaration strings verbatim
// with a PARAMS: header reconstructed from spice_params.
// ─────────────────────────────────────────────────────────────────────────────

std::string CiasCircuitConverter::emit_ltspice_passthrough(const CiasCircuit& circuit) const {
    std::ostringstream out;

    out << ".subckt " << circuit.name;
    for (const auto& p : circuit.ports) out << " " << p.name;

    // Reconstruct PARAMS: header from spice_params (array of ".param ..." lines).
    if (circuit.spice_params.is_array() && !circuit.spice_params.empty()) {
        std::string params_str;
        const std::regex param_prefix(R"(^\s*\.param\s+)", std::regex_constants::icase);
        for (const auto& pl : circuit.spice_params) {
            if (!pl.is_string()) continue;
            std::string s = std::regex_replace(pl.get<std::string>(), param_prefix, "");
            if (!s.empty()) {
                if (!params_str.empty()) params_str += " ";
                params_str += s;
            }
        }
        if (!params_str.empty()) out << " PARAMS: " << params_str;
    }
    out << "\n";

    for (const auto& c : circuit.components) {
        if (!c.data.is_object())
            throw std::runtime_error(
                "CiasCircuitConverter (Ltspice passthrough): component '" + c.name +
                "' has no data object");
        if (!c.data.contains("ltspice_declaration") ||
            !c.data.at("ltspice_declaration").is_string())
            throw std::runtime_error(
                "CiasCircuitConverter (Ltspice passthrough): component '" + c.name +
                "' has no ltspice_declaration — cannot round-trip to LTspice");
        out << c.data.at("ltspice_declaration").get<std::string>() << "\n";
    }

    out << ".ends " << circuit.name << "\n";
    return out.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// PEAS-atom SPICE card generation — renders R/C/L/D/S/K/B cards from PEAS data.
// dialect controls only the behavioural ternary in the integrator block.
// ─────────────────────────────────────────────────────────────────────────────

std::string CiasCircuitConverter::emit_peas_cards(const CiasCircuit& circuit, SpiceDialect dialect) const {
    // Behavioural ternary per dialect: ngspice (c)?(a):(b), LTspice if(c,a,b).
    auto tern = [dialect](const std::string& cond, const std::string& a, const std::string& b) {
        return dialect == SpiceDialect::Ltspice ? ("if(" + cond + "," + a + "," + b + ")")
                                                : ("(" + cond + ")?(" + a + "):(" + b + ")");
    };

    // Build (component, pin) -> node map from the circuit's connections.
    std::map<std::pair<std::string, std::string>, std::string> pinNode;
    for (const auto& conn : circuit.connections) {
        const std::string node = net_node_name(conn);
        for (const auto& ep : conn.endpoints)
            if (ep.isPinEndpoint())
                pinNode[{ep.component, ep.pin}] = node;
    }
    auto node_of = [&](const std::string& comp, const std::string& pin) -> std::string {
        auto it = pinNode.find({comp, pin});
        if (it == pinNode.end())
            // A genuinely-floating pin: its net is touched by a single endpoint in the
            // source, so it is absent from the connection graph (which needs >=2). Emit a
            // unique node name so the pin stays floating — faithful, not an error.
            return "nfloat_" + comp + "_" + pin;
        return it->second;
    };

    std::ostringstream body;
    for (const auto& c : circuit.components) {
        const json& d = c.data;
        if (!d.is_object())
            throw std::runtime_error(
                "CiasCircuitConverter: component '" + c.name + "' has no PEAS data object");

        if (d.contains("resistor")) {
            // Ideal component: electrical value lives in inputs.designRequirements.
            double r = nominal_at(d, {"inputs", "designRequirements", "resistance"},
                                  "resistor " + c.name);
            // A 0 Ohm resistor is a short (e.g. a dc-0 ammeter source we mapped to R).
            // LTspice rejects R=0 ("Resistance must not be zero"); emit a negligible
            // value instead (electrically a short, far below any modelled impedance).
            if (r == 0.0) r = 1e-12;
            body << "R" << c.name << " " << node_of(c.name, "1") << " " << node_of(c.name, "2")
                 << " " << num(r) << "\n";
        }
        else if (d.contains("capacitor")) {
            double cap = nominal_at(d, {"inputs", "designRequirements", "capacitance"},
                                    "capacitor " + c.name);
            body << "C" << c.name << " " << node_of(c.name, "1") << " " << node_of(c.name, "2")
                 << " " << num(cap) << "\n";
        }
        else if (d.contains("semiconductor")) {
            // Faithful emission of the device's SPICE .model card (carried verbatim in
            // spiceModel). No datasheet-derived fabrication; ngspice and LTspice consume
            // .model cards identically.
            const json& semi = d.at("semiconductor");
            std::string devKey;
            for (const char* k : {"diode", "mosfet", "igbt", "bjt"})
                if (semi.contains(k)) { devKey = k; break; }
            if (devKey.empty())
                throw std::runtime_error(
                    "CiasCircuitConverter: semiconductor '" + c.name +
                    "' has no diode/mosfet/igbt/bjt body");
            const json& dev = semi.at(devKey);
            if (!dev.contains("spiceModel"))
                throw std::runtime_error(
                    "CiasCircuitConverter: semiconductor '" + c.name + "' (" + devKey +
                    ") has no spiceModel to emit");
            const json& sm = dev.at("spiceModel");
            const std::string mtype = sm.at("modelType").get<std::string>();
            const std::string model = "MODEL_" + c.name;

            if (devKey == "diode") {
                body << "D" << c.name << " " << node_of(c.name, "anode") << " "
                     << node_of(c.name, "cathode") << " " << model << "\n";
            } else if (devKey == "bjt") {
                body << "Q" << c.name << " " << node_of(c.name, "collector") << " "
                     << node_of(c.name, "base") << " " << node_of(c.name, "emitter")
                     << " " << model << "\n";
            } else if (devKey == "mosfet") {
                body << "M" << c.name << " " << node_of(c.name, "drain") << " "
                     << node_of(c.name, "gate") << " " << node_of(c.name, "source") << " "
                     << node_of(c.name, "source") << " " << model << "\n";
            } else {
                throw std::runtime_error(
                    "CiasCircuitConverter: IGBT primitive emit not supported for '" + c.name + "'");
            }

            body << ".model " << model << " " << mtype;
            if (sm.contains("parameters") && sm.at("parameters").is_object()
                && !sm.at("parameters").empty()) {
                body << "(";
                for (auto it = sm.at("parameters").begin(); it != sm.at("parameters").end(); ++it) {
                    const json& val = it.value();
                    body << it.key() << "=";
                    if (val.is_number()) body << num(val.get<double>());
                    else if (val.is_string()) body << val.get<std::string>();
                    else body << val.dump();
                    body << " ";
                }
                body << ")";
            }
            body << "\n";
        }
        else if (d.contains("magnetic")) {
            const json& mag = d.at("magnetic");
            // MKF_MODEL path: the magnetic carries a pre-fitted MKF-exported SPICE subcircuit
            // (real winding Rdc + AC-resistance ladder + magnetizing L + leakage coupling).
            // The assembler hoists the .subckt definition; here we emit only the X instance.
            if (mag.contains("modelOutputs") && mag.at("modelOutputs").contains("spiceSubcircuit")) {
                const json& sk = mag.at("modelOutputs").at("spiceSubcircuit");
                size_t nsec = 0;
                if (d.at("inputs").at("designRequirements").contains("turnsRatios"))
                    nsec = d.at("inputs").at("designRequirements").at("turnsRatios").size();
                body << "X" << c.name << " " << node_of(c.name, "primary_start")
                     << " " << node_of(c.name, "primary_end");
                for (size_t i = 0; i < nsec; ++i) {
                    const std::string idx = std::to_string(i + 1);
                    body << " " << node_of(c.name, "secondary" + idx + "_start")
                         << " " << node_of(c.name, "secondary" + idx + "_end");
                }
                body << " " << sk.at("reference").get<std::string>() << "\n";
            }
            else {
                // Ideal path: one L per winding + pairwise K coupling.
                const double lp = nominal_at(d, {"inputs", "designRequirements", "magnetizingInductance"},
                                             "magnetic " + c.name);
                std::vector<double> ratios;
                if (const json* tr = (d.at("inputs").at("designRequirements").contains("turnsRatios")
                                      ? &d.at("inputs").at("designRequirements").at("turnsRatios")
                                      : nullptr)) {
                    for (const auto& e : *tr)
                        ratios.push_back(e.is_object() && e.contains("nominal")
                                         ? e.at("nominal").get<double>() : e.get<double>());
                }
                std::vector<std::string> indNames;
                const std::string lpri = "L" + c.name + "_pri";
                body << lpri << " " << node_of(c.name, "primary_start") << " "
                     << node_of(c.name, "primary_end") << " " << num(lp) << "\n";
                indNames.push_back(lpri);

                for (size_t i = 0; i < ratios.size(); ++i) {
                    if (ratios[i] == 0.0)
                        throw std::runtime_error(
                            "CiasCircuitConverter: zero turns ratio in magnetic " + c.name);
                    const double ls = lp / (ratios[i] * ratios[i]);
                    const std::string idx = std::to_string(i + 1);
                    const std::string lsec = "L" + c.name + "_sec" + idx;
                    body << lsec << " " << node_of(c.name, "secondary" + idx + "_start") << " "
                         << node_of(c.name, "secondary" + idx + "_end") << " " << num(ls) << "\n";
                    indNames.push_back(lsec);
                }

                // Coupling derives from per-winding leakage inductance: Lleak_i = Lp*(1-k_i^2),
                // referred to the primary, so k_i = sqrt(1 - Lleak_i/Lp). Absent leakage means
                // ideal (k=1). ngspice cannot solve k==1 (singular mutual matrix), so the EMITTED
                // value is capped just below unity (a simulator workaround, not a data default).
                const json& mdr = d.at("inputs").at("designRequirements");
                std::vector<double> kToPri(indNames.size(), 1.0);  // index 0 = primary
                if (mdr.contains("leakageInductance") && mdr.at("leakageInductance").is_array()) {
                    const json& lks = mdr.at("leakageInductance");
                    for (size_t i = 0; i < lks.size() && i + 1 < kToPri.size(); ++i) {
                        const json& e = lks[i];
                        double lk = (e.is_object() && e.contains("nominal"))
                                        ? e.at("nominal").get<double>() : e.get<double>();
                        double ratio = lp > 0.0 ? lk / lp : 0.0;
                        if (ratio < 0.0) ratio = 0.0;
                        if (ratio > 1.0) ratio = 1.0;
                        kToPri[i + 1] = std::sqrt(1.0 - ratio);
                    }
                }
                for (size_t i = 0; i < indNames.size(); ++i)
                    for (size_t j = i + 1; j < indNames.size(); ++j) {
                        double kij = (i == 0) ? kToPri[j] : std::min(kToPri[i], kToPri[j]);
                        double kEmit = (dialect == SpiceDialect::Ltspice) ? kij : std::min(kij, 0.999999);
                        body << "K" << c.name << "_" << i << j << " " << indNames[i] << " "
                             << indNames[j] << " " << num(kEmit) << "\n";
                    }
            }
        }
        else if (d.contains("analog")) {
            const json& aas = d.at("analog");
            if (aas.contains("comparator")) {
                const json& cmp = aas.at("comparator");
                auto opt = [&](const char* key, double dflt) -> double {
                    if (cmp.contains("behavioral") && cmp.at("behavioral").is_object()
                        && cmp.at("behavioral").contains(key)) {
                        const json& v = cmp.at("behavioral").at(key);
                        if (v.is_number()) return v.get<double>();
                        if (v.is_object() && v.contains("nominal")) return v.at("nominal").get<double>();
                    }
                    return dflt;
                };
                const double vHigh = opt("outputHigh", 5.0);
                const double vLow  = opt("outputLow", 0.0);
                const double thr   = opt("threshold", 0.0);
                const double hyst  = opt("hysteresis", 0.0);
                const std::string inP = node_of(c.name, "inPlus");
                const std::string inN = node_of(c.name, "inMinus");
                const std::string out = node_of(c.name, "out");
                const std::string hiRail = c.name + "__vh";
                const std::string loRail = (vLow != 0.0) ? (c.name + "__vl") : "0";
                const std::string model  = "CMP_" + c.name;
                body << "V" << c.name << "_vh " << hiRail << " 0 " << num(vHigh) << "\n";
                if (vLow != 0.0) body << "V" << c.name << "_vl " << loRail << " 0 " << num(vLow) << "\n";
                body << "S" << c.name << " " << out << " " << hiRail << " " << inP << " " << inN
                     << " " << model << "\n";
                body << "R" << c.name << "_pd " << out << " " << loRail << " 1k\n";
                body << ".model " << model << " SW(Vt=" << num(thr) << " Vh=" << num(std::max(hyst, 0.0))
                     << " Ron=1 Roff=1e9)\n";
            }
            else if (aas.contains("multiplier")) {
                const json& m = aas.at("multiplier");
                double gain = 1.0;
                if (m.contains("behavioral") && m.at("behavioral").is_object()
                    && m.at("behavioral").contains("gain"))
                    gain = m.at("behavioral").at("gain").get<double>();
                const std::string inA = node_of(c.name, "inA");
                const std::string inB = node_of(c.name, "inB");
                const std::string out = node_of(c.name, "out");
                body << "B" << c.name << " " << out << " 0 V=" << num(gain) << "*V(" << inA << ")*V("
                     << inB << ")\n";
            }
            else if (aas.contains("summer")) {
                const json& s = aas.at("summer");
                auto opt = [&](const char* key, double dflt) -> double {
                    if (s.contains("behavioral") && s.at("behavioral").is_object()
                        && s.at("behavioral").contains(key))
                        return s.at("behavioral").at(key).get<double>();
                    return dflt;
                };
                const double gA = opt("gainA", 1.0), gB = opt("gainB", -1.0);
                const std::string inA = node_of(c.name, "inA");
                const std::string inB = node_of(c.name, "inB");
                const std::string out = node_of(c.name, "out");
                body << "B" << c.name << " " << out << " 0 V=" << num(gA) << "*V(" << inA << ")+"
                     << num(gB) << "*V(" << inB << ")\n";
            }
            else if (aas.contains("integrator")) {
                const json& it = aas.at("integrator");
                auto opt = [&](const char* key, double dflt) -> double {
                    if (it.contains("behavioral") && it.at("behavioral").is_object()
                        && it.at("behavioral").contains(key))
                        return it.at("behavioral").at(key).get<double>();
                    return dflt;
                };
                const double gain = opt("gain", 1.0), initial = opt("initial", 0.0);
                const double ref = opt("reference", 0.0);
                const double lo = opt("outputLow", -1e9), hi = opt("outputHigh", 1e9);
                const std::string in   = node_of(c.name, "in");
                const std::string out  = node_of(c.name, "out");
                const std::string raw  = c.name + "__raw";
                const bool clamped = (lo > -1e8) || (hi < 1e8);
                const std::string vr = "V(" + raw + ")";
                body << "B" << c.name << "_i 0 " << raw << " I=" << num(gain) << "*(V(" << in << ")-("
                     << num(ref) << "))";
                if (clamped) {
                    const double kAw = 1e4;
                    const std::string overHi = tern(vr + ">(" + num(hi) + ")", vr + "-(" + num(hi) + ")", "0");
                    const std::string undLo  = tern(vr + "<(" + num(lo) + ")", vr + "-(" + num(lo) + ")", "0");
                    body << "-" << num(kAw) << "*((" << overHi << ")+(" << undLo << "))";
                }
                body << "\n";
                body << "C" << c.name << "_int " << raw << " 0 1 IC=" << num(initial) << "\n";
                const std::string clampExpr =
                    tern(vr + "<(" + num(lo) + ")", num(lo), tern(vr + ">(" + num(hi) + ")", num(hi), vr));
                body << "B" << c.name << " " << out << " 0 V=" << clampExpr << "\n";
            }
            else {
                throw std::runtime_error(
                    "CiasCircuitConverter: analog '" + c.name +
                    "' block type not supported (comparator/multiplier/summer/integrator only)");
            }
        }
        else if (d.contains("behavioral")) {
            const json& beh = d.at("behavioral");
            const std::string nature = beh.at("nature").get<std::string>();
            const std::string n1 = node_of(c.name, "1");
            const std::string n2 = node_of(c.name, "2");
            // Helper: replace every whole-word 'i' with I(sense) and 'v' with V(n1,n2).
            auto subst_vars = [&](std::string expr,
                                  const std::string& sense_name,
                                  const std::string& na, const std::string& nb) -> std::string {
                expr = std::regex_replace(expr, std::regex(R"(\bi\b)"), "I(" + sense_name + ")");
                expr = std::regex_replace(expr, std::regex(R"(\bv\b)"), "V(" + na + "," + nb + ")");
                return expr;
            };

            if (nature == "flux") {
                const std::string expr = beh.at("expression").get<std::string>();
                if (dialect == SpiceDialect::Ltspice) {
                    // Restore LTspice FLux= syntax: i→x (branch variable).
                    std::string lt_expr = std::regex_replace(expr, std::regex(R"(\bi\b)"), "x");
                    body << "L" << c.name << " " << n1 << " " << n2
                         << " Flux=" << lt_expr << "\n";
                } else {
                    // ngspice ≥43: B-element with ddt() + Vsense current sensor.
                    // Requires: .options method=gear  (stiff ODE; avoids trapezoidal ringing)
                    const std::string sense = "Vsense_" + c.name;
                    const std::string n_int = n1 + "__" + c.name;
                    body << sense << " " << n_int << " " << n1 << " DC=0\n";
                    body << "B" << c.name << " " << n_int << " " << n2
                         << " V=ddt(" << subst_vars(expr, sense, n1, n2) << ")\n";
                }
            }
            else if (nature == "charge") {
                const std::string expr = beh.at("expression").get<std::string>();
                if (dialect == SpiceDialect::Ltspice) {
                    // Restore LTspice Q= syntax: v→x.
                    std::string lt_expr = std::regex_replace(expr, std::regex(R"(\bv\b)"), "x");
                    body << "C" << c.name << " " << n1 << " " << n2
                         << " Q=" << lt_expr << "\n";
                } else {
                    // ngspice ≥43: B current source with ddt() of charge expression.
                    // We need port current for 'i' — use the sibling Vsense if present,
                    // else insert a dedicated sense element.
                    const std::string sense = "Vsense_" + c.name;
                    const std::string n_int = n1 + "__q" + c.name;
                    body << sense << " " << n_int << " " << n1 << " DC=0\n";
                    body << "B" << c.name << " " << n_int << " " << n2
                         << " I=ddt(" << subst_vars(expr, sense, n1, n2) << ")\n";
                }
            }
            else if (nature == "controlled") {
                // Controlled/dependent source: value = f(own terminals). Emit a B-source
                // between the 'across' pins; substitute i(p,q)->I(Vsense) (synthesising a
                // 0 V sense per distinct i()) and v(p,q)->V(node,node); pwl()->table() for LTspice.
                const json& out = beh.at("output");
                const std::string quantity = out.at("quantity").get<std::string>();
                const json& across = out.at("across");
                const std::string na = node_of(c.name, across[0].get<std::string>());
                const std::string nb = node_of(c.name, across[1].get<std::string>());
                std::string expr = out.at("expression").get<std::string>();

                // Linear VCCS/VCVS '(K)*v(p,q)' -> NATIVE G/E element (exact). A behavioral
                // B-source is mathematically equivalent but carries a small per-element error
                // that compounds catastrophically in tightly-coupled networks (e.g. a CM choke
                // with 1000+ gyrator sources). Native E/G has no such error.
                {
                    std::smatch lm;
                    if (std::regex_match(expr, lm,
                            std::regex(R"(^\(\s*([-+0-9.eE]+)\s*\)\s*\*\s*v\(\s*(\w+)\s*,\s*(\w+)\s*\)$)",
                                       std::regex::icase))) {
                        const std::string kgain = lm[1].str();
                        const std::string cp = node_of(c.name, lm[2].str());
                        const std::string cn = node_of(c.name, lm[3].str());
                        body << (quantity == "current" ? "G" : "E") << c.name << " "
                             << na << " " << nb << " " << cp << " " << cn << " " << kgain << "\n";
                        continue;
                    }
                }

                auto subst = [&](const std::string& fn, bool isCurrent,
                                 std::ostringstream& senses, int& k) {
                    std::regex re("\\b" + fn + "\\s*\\(\\s*(\\w+)\\s*,\\s*(\\w+)\\s*\\)",
                                  std::regex::icase);
                    std::string res;
                    auto last = expr.cbegin();
                    for (auto it = std::sregex_iterator(expr.begin(), expr.end(), re);
                         it != std::sregex_iterator(); ++it) {
                        const std::smatch& m = *it;
                        res.append(last, expr.cbegin() + m.position());
                        const std::string p = node_of(c.name, m[1].str());
                        const std::string q = node_of(c.name, m[2].str());
                        if (isCurrent) {
                            const std::string sense = "Vsns_" + c.name + "_" + std::to_string(k++);
                            senses << sense << " " << p << " " << q << " DC 0\n";
                            res += "I(" + sense + ")";
                        } else {
                            res += "V(" + p + "," + q + ")";
                        }
                        last = expr.cbegin() + m.position() + m.length();
                    }
                    res.append(last, expr.cend());
                    expr = res;
                };
                std::ostringstream senses;
                int k = 0;
                subst("i", true, senses, k);   // i(p,q) -> I(Vsense)
                subst("v", false, senses, k);  // v(p,q) -> V(node,node)
                if (dialect == SpiceDialect::Ltspice)
                    expr = std::regex_replace(expr, std::regex(R"(\bpwl\s*\()", std::regex::icase),
                                              "table(");
                body << senses.str();
                body << "B" << c.name << " " << na << " " << nb << " "
                     << (quantity == "current" ? "I=" : "V=") << expr << "\n";
            }
            else if (nature == "chan") {
                // Chan saturable core (LTspice Hc/Bs/Br material + A/Lm/Lg/N geometry).
                const json& p = beh.at("parameters");
                const std::string ps  = node_of(c.name, "primary_start");
                const std::string pe  = node_of(c.name, "primary_end");
                if (dialect == SpiceDialect::Ltspice) {
                    // B-H params (Hc/Bs/Br) may be temperature curves → rebuild tbl(temp,...).
                    auto bhval = [&](const char* key) -> std::string {
                        const json& v = p.at(key);
                        if (v.is_number()) return num(v.get<double>());
                        // Temperature curve -> tbl(temp,...). LTspice requires an expression
                        // attribute to be braced ({...}); a bare tbl() on Hc=/Bs=/Br= is read
                        // as 0 ("missing coercive force").
                        std::ostringstream t;
                        t << "{tbl(temp";
                        for (const auto& pt : v.at("perTemperature"))
                            t << "," << num(pt.at("temperature").get<double>())
                              << "," << num(pt.at("value").get<double>());
                        t << ")}";
                        return t.str();
                    };
                    body << "L" << c.name << " " << ps << " " << pe
                         << " Hc=" << bhval("coercive_force")
                         << " Bs=" << bhval("saturation_flux_density")
                         << " Br=" << bhval("remanence_flux_density")
                         << " A="  << num(p.at("effective_area").get<double>())
                         << " Lm=" << num(p.at("magnetic_path_length").get<double>())
                         << " Lg=" << num(p.at("air_gap").get<double>())
                         << " N="  << num(p.at("turns").get<double>()) << "\n";
                } else {
                    // ngspice has no native Chan core. Emit the Rank-2 flux-based behavioral
                    // model: a B-source computes the Chan flux linkage Psi(i) from the closed-
                    // form B-H major loop, and a 1 F integrator differentiates it (V = dPsi/dt).
                    // This reproduces the SATURATION curve exactly (anhysteretic; the hysteresis
                    // loss loop is NOT modelled — that needs a Verilog-A/OSDI module). B-H params
                    // (which may be temperature curves) are resolved at their referenceTemperature.
                    auto bh = [&](const char* key) -> double {
                        const json& v = p.at(key);
                        if (v.is_number()) return v.get<double>();
                        const json& pts = v.at("perTemperature");
                        double Tref = v.contains("referenceTemperature")
                                          ? v.at("referenceTemperature").get<double>() : 25.0;
                        double t0 = pts[0].at("temperature").get<double>();
                        double v0 = pts[0].at("value").get<double>();
                        if (Tref <= t0) return v0;
                        for (size_t k = 1; k < pts.size(); ++k) {
                            double t1 = pts[k].at("temperature").get<double>();
                            double v1 = pts[k].at("value").get<double>();
                            if (Tref <= t1) return v0 + (Tref - t0) / (t1 - t0) * (v1 - v0);
                            t0 = t1; v0 = v1;
                        }
                        return v0;
                    };
                    const double Hc = bh("coercive_force");
                    const double Bs = bh("saturation_flux_density");
                    const double Br = bh("remanence_flux_density");
                    const double A  = p.at("effective_area").get<double>();
                    const double Lm = p.at("magnetic_path_length").get<double>();
                    const double N  = p.at("turns").get<double>();
                    const std::string sense = "Vsns_" + c.name;
                    const std::string ni    = ps + "__i" + c.name;
                    const std::string npsi  = c.name + "__psi";
                    const std::string ndpsi = c.name + "__dpsi";
                    const std::string vc    = "Vint_" + c.name;
                    // H = N*i/Lm ; Chan rational major-loop branches; Psi = N*A*0.5*(Bdn+Bup).
                    const std::string H   = "(" + num(N) + "*i(" + sense + ")/" + num(Lm) + ")";
                    const std::string den = num(Hc) + "*(" + num(Bs) + "/" + num(Br) + "-1)";
                    const std::string Bdn = num(Bs) + "*((" + H + ")-" + num(Hc) + ")/(abs((" + H
                                          + ")-" + num(Hc) + ")+" + den + ")";
                    const std::string Bup = num(Bs) + "*((" + H + ")+" + num(Hc) + ")/(abs((" + H
                                          + ")+" + num(Hc) + ")+" + den + ")";
                    body << sense << " " << ps << " " << ni << " DC 0\n";
                    body << "B" << c.name << "_psi " << npsi << " 0 V=" << num(N) << "*" << num(A)
                         << "*0.5*((" << Bdn << ")+(" << Bup << "))\n";
                    body << "C" << c.name << "_int " << npsi << " " << ndpsi << " 1\n";
                    body << vc << " " << ndpsi << " 0 DC 0\n";
                    body << "H" << c.name << " " << ni << " " << pe << " " << vc << " 1\n";
                }
            }
            else {
                throw std::runtime_error(
                    "CiasCircuitConverter: behavioral component '" + c.name +
                    "' has unknown nature '" + nature + "' — expected flux/charge/chan");
            }
        }
        else {
            throw std::runtime_error(
                "CiasCircuitConverter: component '" + c.name +
                "' has an unknown PEAS discriminator — expected resistor/capacitor/magnetic/"
                "semiconductor/analog/behavioral");
        }
    }
    return body.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

std::string CiasCircuitConverter::to_cards(const CiasCircuit& circuit) const {
    if (target_ == CircuitSimulator::Ngspice || target_ == CircuitSimulator::Ltspice)
        return emit_peas_cards(circuit, spice_dialect());
    throw std::runtime_error(
        "CiasCircuitConverter::to_cards not available for " + simulator_name(target_) +
        " (PEAS-atom card emission only supports Ngspice and Ltspice)");
}

std::string CiasCircuitConverter::to_subckt(const CiasCircuit& circuit) const {
    // LTspice passthrough: if any component carries a ltspice_declaration, treat the whole
    // circuit as a library-extracted circuit and round-trip via the original declarations.
    if (target_ == CircuitSimulator::Ltspice && has_ltspice_declarations(circuit))
        return emit_ltspice_passthrough(circuit);

    if (target_ == CircuitSimulator::Ngspice || target_ == CircuitSimulator::Ltspice) {
        std::ostringstream out;
        out << ".subckt " << circuit.name;
        for (const auto& p : circuit.ports) out << " " << p.name;
        out << "\n";
        out << emit_peas_cards(circuit, spice_dialect());
        out << ".ends " << circuit.name << "\n";
        return out.str();
    }

    throw std::runtime_error(
        "CiasCircuitConverter: simulator '" + simulator_name(target_) +
        "' is not yet implemented");
}

std::string CiasCircuitConverter::to_subckt_json(const json& ciasJson) const {
    return to_subckt(CiasCircuit::from_json(ciasJson));
}

// ─────────────────────────────────────────────────────────────────────────────
// Structural validator (graph-level invariants the JSON Schema cannot express)
// ─────────────────────────────────────────────────────────────────────────────

std::vector<std::string> validate_cias_structure(const CiasCircuit& circuit) {
    std::vector<std::string> problems;
    if (circuit.name.empty()) problems.push_back("circuit has an empty name");

    std::set<std::string> portNames, compNames, connNames;
    for (const auto& p : circuit.ports) {
        if (p.name.empty()) problems.push_back("a port has an empty name");
        else if (!portNames.insert(p.name).second)
            problems.push_back("duplicate port name '" + p.name + "'");
    }

    static const std::vector<std::string> KNOWN = {
        "resistor", "capacitor", "magnetic", "semiconductor",
        "controller", "connector", "analog", "behavioral"};
    for (const auto& c : circuit.components) {
        if (c.name.empty()) problems.push_back("a component has an empty name");
        else if (!compNames.insert(c.name).second)
            problems.push_back("duplicate component name '" + c.name + "'");
        if (c.data.is_string()) continue;  // URI reference into a part-data file
        if (!c.data.is_object()) {
            problems.push_back("component '" + c.name + "' data is neither object nor URI");
            continue;
        }
        int disc = 0;
        for (const auto& k : KNOWN) if (c.data.contains(k)) ++disc;
        if (disc != 1)
            problems.push_back("component '" + c.name + "' has " + std::to_string(disc) +
                               " discriminators (expected exactly 1)");
        if (!c.data.contains("inputs"))
            problems.push_back("component '" + c.name + "' has no inputs");
    }

    for (const auto& conn : circuit.connections) {
        if (conn.name.empty()) problems.push_back("a connection has an empty name");
        else if (!connNames.insert(conn.name).second)
            problems.push_back("duplicate connection name '" + conn.name + "'");
        if (conn.endpoints.size() < 2)
            problems.push_back("connection '" + conn.name + "' has fewer than 2 endpoints");
        for (const auto& ep : conn.endpoints) {
            if (ep.isPinEndpoint()) {
                if (!compNames.count(ep.component))
                    problems.push_back("connection '" + conn.name +
                                       "' references unknown component '" + ep.component + "'");
            } else if (ep.isPortEndpoint()) {
                if (!portNames.count(ep.port))
                    problems.push_back("connection '" + conn.name +
                                       "' references unknown port '" + ep.port + "'");
            } else {
                problems.push_back("connection '" + conn.name + "' has a malformed endpoint");
            }
        }
    }
    return problems;
}

} // namespace CIAS
