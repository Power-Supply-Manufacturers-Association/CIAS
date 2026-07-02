#include "CiasCircuitConverter.hpp"
#include "DimensionJson.hpp"   // PEAS::resolve_dimensional_values (json overload)
#include <sstream>
#include <map>
#include <set>
#include <vector>
#include <cmath>
#include <algorithm>
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
// connection's own (brick-local) name. A net exposed at TWO ports is an error: one net can carry
// only one node name, and silently picking one leaves the other .subckt header port dangling.
std::string net_node_name(const Connection& conn) {
    std::string portName;
    for (const auto& ep : conn.endpoints) {
        if (ep.isPortEndpoint()) {
            if (!portName.empty())
                throw std::runtime_error(
                    "CiasCircuitConverter: net '" + conn.name + "' is exposed at two ports ('" +
                    portName + "' and '" + ep.port + "') — tie two brick ports together with an "
                    "explicit 0 V source component instead");
            portName = ep.port;
        }
    }
    return portName.empty() ? conn.name : portName;
}

// Traverse data[keys...] and resolve a dimensionWithTolerance (or bare number) to a scalar
// with the canonical PEAS semantics (nominal -> mean(min,max) -> max -> min; throws when empty).
double resolved_at(const json& data, std::initializer_list<const char*> keys, const std::string& what) {
    const json* cur = &data;
    for (const char* k : keys) {
        if (!cur->is_object() || !cur->contains(k))
            throw std::runtime_error("CiasCircuitConverter: missing '" + std::string(k) + "' for " + what);
        cur = &cur->at(k);
    }
    try {
        return PEAS::resolve_dimensional_values(*cur);
    } catch (const std::exception&) {
        throw std::runtime_error("CiasCircuitConverter: no numeric value (nominal/minimum/maximum) for " + what);
    }
}

// Resolve a leaf json value that may be a bare number or a dimensionWithTolerance.
double resolved_leaf(const json& v, const std::string& what) {
    try {
        return PEAS::resolve_dimensional_values(v);
    } catch (const std::exception&) {
        throw std::runtime_error("CiasCircuitConverter: no numeric value for " + what);
    }
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

// Sibling current reference: i(<designator>) -> I(V<designator>). Source-nature components are
// emitted as V<name>/I<name> elements; a 0 V source is an ideal ammeter whose branch current a
// sibling behavioral expression references as i(<designator>) (see the PEAS `source` nature).
std::string subst_sibling_currents(const std::string& expr) {
    static const std::regex re(R"(\bi\s*\(\s*([A-Za-z_]\w*)\s*\))");
    return std::regex_replace(expr, re, "I(V$1)");
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / factory
// ─────────────────────────────────────────────────────────────────────────────

CiasCircuitConverter::CiasCircuitConverter(CircuitSimulator target) : target_(target) {}

std::shared_ptr<CiasCircuitConverter> CiasCircuitConverter::create(CircuitSimulator target) {
    return std::make_shared<CiasCircuitConverter>(target);
}

SpiceDialect CiasCircuitConverter::spice_dialect() const {
    return target_ == CircuitSimulator::Ltspice ? SpiceDialect::Ltspice : SpiceDialect::Ngspice;
}

// ─────────────────────────────────────────────────────────────────────────────
// PEAS-atom SPICE card generation — renders R/C/L/D/Q/M/S/K/B/V/I cards from PEAS data.
// ─────────────────────────────────────────────────────────────────────────────

std::string CiasCircuitConverter::emit_peas_cards(const CiasCircuit& circuit, SpiceDialect dialect) const {
    // Behavioural ternary per dialect: ngspice (c)?(a):(b), LTspice if(c,a,b).
    auto tern = [dialect](const std::string& cond, const std::string& a, const std::string& b) {
        return dialect == SpiceDialect::Ltspice ? ("if(" + cond + "," + a + "," + b + ")")
                                                : ("(" + cond + ")?(" + a + "):(" + b + ")");
    };

    // Build (component, pin) -> node map and component -> used-pin set from the connections.
    std::map<std::pair<std::string, std::string>, std::string> pinNode;
    std::map<std::string, std::set<std::string>> usedPins;
    for (const auto& conn : circuit.connections) {
        const std::string node = net_node_name(conn);
        for (const auto& ep : conn.endpoints)
            if (ep.isPinEndpoint()) {
                pinNode[{ep.component, ep.pin}] = node;
                usedPins[ep.component].insert(ep.pin);
            }
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
    // Guard against typo'd pin names in connections: a connection referencing pin "gat" of a
    // MOSFET would otherwise silently leave the real "gate" floating.
    auto check_pins = [&](const std::string& comp, const std::string& kind,
                          const std::set<std::string>& allowed) {
        auto it = usedPins.find(comp);
        if (it == usedPins.end()) return;
        for (const auto& p : it->second)
            if (!allowed.count(p)) {
                std::string exp;
                for (const auto& a : allowed) exp += (exp.empty() ? "" : "/") + a;
                throw std::runtime_error(
                    "CiasCircuitConverter: connection references unknown pin '" + p + "' of " +
                    kind + " '" + comp + "' (expected: " + exp + ")");
            }
    };

    std::ostringstream body;
    for (const auto& c : circuit.components) {
        const json& d = c.data;
        if (!d.is_object())
            throw std::runtime_error(
                "CiasCircuitConverter: component '" + c.name + "' has no PEAS data object");

        if (d.contains("resistor")) {
            check_pins(c.name, "resistor", {"1", "2"});
            // Ideal component: electrical value lives in inputs.designRequirements.
            double r = resolved_at(d, {"inputs", "designRequirements", "resistance"},
                                   "resistor " + c.name);
            // A 0 Ohm resistor is a short (e.g. a dc-0 ammeter source we mapped to R).
            // LTspice rejects R=0 ("Resistance must not be zero") and ngspice's handling is
            // version-dependent; emit a negligible value instead (electrically a short, far
            // below any modelled impedance). Realization workaround, not a data default.
            if (r == 0.0) r = 1e-12;
            body << "R" << c.name << " " << node_of(c.name, "1") << " " << node_of(c.name, "2")
                 << " " << num(r) << "\n";
        }
        else if (d.contains("capacitor")) {
            check_pins(c.name, "capacitor", {"1", "2"});
            double cap = resolved_at(d, {"inputs", "designRequirements", "capacitance"},
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
                check_pins(c.name, "diode", {"anode", "cathode"});
                body << "D" << c.name << " " << node_of(c.name, "anode") << " "
                     << node_of(c.name, "cathode") << " " << model << "\n";
            } else if (devKey == "bjt") {
                check_pins(c.name, "bjt", {"collector", "base", "emitter"});
                body << "Q" << c.name << " " << node_of(c.name, "collector") << " "
                     << node_of(c.name, "base") << " " << node_of(c.name, "emitter")
                     << " " << model << "\n";
            } else if (devKey == "mosfet") {
                check_pins(c.name, "mosfet", {"drain", "gate", "source"});
                std::string mt = mtype;
                std::transform(mt.begin(), mt.end(), mt.begin(), ::tolower);
                body << "M" << c.name << " " << node_of(c.name, "drain") << " "
                     << node_of(c.name, "gate") << " " << node_of(c.name, "source");
                // VDMOS (the LTspice power-FET model, supported by ngspice too) is a
                // 3-terminal model: a 4th (bulk) node makes the card invalid.
                if (mt != "vdmos")
                    body << " " << node_of(c.name, "source");
                body << " " << model << "\n";
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
            size_t nsec = 0;
            if (d.at("inputs").at("designRequirements").contains("turnsRatios"))
                nsec = d.at("inputs").at("designRequirements").at("turnsRatios").size();
            {
                std::set<std::string> allowed = {"primary_start", "primary_end"};
                for (size_t i = 1; i <= nsec; ++i) {
                    allowed.insert("secondary" + std::to_string(i) + "_start");
                    allowed.insert("secondary" + std::to_string(i) + "_end");
                }
                check_pins(c.name, "magnetic", allowed);
            }
            // MKF_MODEL path: the magnetic carries a pre-fitted MKF-exported SPICE subcircuit
            // (real winding Rdc + AC-resistance ladder + magnetizing L + leakage coupling).
            // The assembler hoists the .subckt definition; here we emit only the X instance.
            if (mag.contains("modelOutputs") && mag.at("modelOutputs").contains("spiceSubcircuit")) {
                const json& sk = mag.at("modelOutputs").at("spiceSubcircuit");
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
                const double lp = resolved_at(d, {"inputs", "designRequirements", "magnetizingInductance"},
                                              "magnetic " + c.name);
                if (lp <= 0.0)
                    throw std::runtime_error(
                        "CiasCircuitConverter: magnetic '" + c.name +
                        "' has non-positive magnetizingInductance " + num(lp));
                std::vector<double> ratios;
                if (const json* tr = (d.at("inputs").at("designRequirements").contains("turnsRatios")
                                      ? &d.at("inputs").at("designRequirements").at("turnsRatios")
                                      : nullptr)) {
                    for (const auto& e : *tr)
                        ratios.push_back(resolved_leaf(e, "turns ratio of magnetic " + c.name));
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
                        const double lk = resolved_leaf(lks[i], "leakage inductance of magnetic " + c.name);
                        if (lk < 0.0)
                            throw std::runtime_error(
                                "CiasCircuitConverter: negative leakage inductance in magnetic " + c.name);
                        if (lk >= lp)
                            throw std::runtime_error(
                                "CiasCircuitConverter: leakage inductance >= magnetizing inductance "
                                "in magnetic '" + c.name + "' (" + num(lk) + " >= " + num(lp) +
                                ") — corrupt data");
                        kToPri[i + 1] = std::sqrt(1.0 - lk / lp);
                    }
                }
                for (size_t i = 0; i < indNames.size(); ++i)
                    for (size_t j = i + 1; j < indNames.size(); ++j) {
                        // sec-sec coupling goes through the shared primary path: k_ij = k_i*k_j.
                        double kij = (i == 0) ? kToPri[j] : kToPri[i] * kToPri[j];
                        double kEmit = (dialect == SpiceDialect::Ltspice) ? kij : std::min(kij, 0.999999);
                        body << "K" << c.name << "_" << i << "_" << j << " " << indNames[i] << " "
                             << indNames[j] << " " << num(kEmit) << "\n";
                    }
            }
        }
        else if (d.contains("analog")) {
            const json& aas = d.at("analog");
            // Realization of an AAS analog block requires its `behavioral` model. A datasheet-only
            // part must NOT silently become a fabricated ideal block (no-fallbacks rule).
            auto behavioral_of = [&](const json& blk, const char* kind) -> const json& {
                if (!blk.contains("behavioral") || !blk.at("behavioral").is_object())
                    throw std::runtime_error(
                        "CiasCircuitConverter: analog " + std::string(kind) + " '" + c.name +
                        "' has no behavioral block — cannot realize a datasheet-only part as an "
                        "ideal element");
                return blk.at("behavioral");
            };
            auto required = [&](const json& b, const char* key, const char* kind) -> double {
                if (!b.contains(key))
                    throw std::runtime_error(
                        "CiasCircuitConverter: analog " + std::string(kind) + " '" + c.name +
                        "' behavioral block is missing required '" + key + "'");
                return resolved_leaf(b.at(key), std::string(key) + " of " + c.name);
            };
            auto optional_with_documented_default = [&](const json& b, const char* key,
                                                        double documented) -> double {
                if (!b.contains(key)) return documented;
                return resolved_leaf(b.at(key), std::string(key) + " of " + c.name);
            };

            if (aas.contains("comparator")) {
                check_pins(c.name, "comparator", {"inPlus", "inMinus", "out"});
                const json& b = behavioral_of(aas.at("comparator"), "comparator");
                const double vHigh = required(b, "outputHigh", "comparator");
                const double vLow  = required(b, "outputLow", "comparator");
                // threshold: trip point of out = (V+ - V- > threshold); absent = 0 per the
                // schema's stated semantics. hysteresis: documented "0 (or absent) is ideal".
                const double thr  = optional_with_documented_default(b, "threshold", 0.0);
                const double hyst = optional_with_documented_default(b, "hysteresis", 0.0);
                if (hyst < 0.0)
                    throw std::runtime_error(
                        "CiasCircuitConverter: comparator '" + c.name + "' has negative hysteresis");
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
                body << ".model " << model << " SW(Vt=" << num(thr) << " Vh=" << num(hyst)
                     << " Ron=1 Roff=1e9)\n";
            }
            else if (aas.contains("multiplier")) {
                check_pins(c.name, "multiplier", {"inA", "inB", "out"});
                const json& b = behavioral_of(aas.at("multiplier"), "multiplier");
                const double gain = required(b, "gain", "multiplier");
                const std::string inA = node_of(c.name, "inA");
                const std::string inB = node_of(c.name, "inB");
                const std::string out = node_of(c.name, "out");
                body << "B" << c.name << " " << out << " 0 V=" << num(gain) << "*V(" << inA << ")*V("
                     << inB << ")\n";
            }
            else if (aas.contains("summer")) {
                check_pins(c.name, "summer", {"inA", "inB", "out"});
                const json& b = behavioral_of(aas.at("summer"), "summer");
                const double gA = required(b, "gainA", "summer");
                const double gB = required(b, "gainB", "summer");
                const std::string inA = node_of(c.name, "inA");
                const std::string inB = node_of(c.name, "inB");
                const std::string out = node_of(c.name, "out");
                body << "B" << c.name << " " << out << " 0 V=" << num(gA) << "*V(" << inA << ")+"
                     << num(gB) << "*V(" << inB << ")\n";
            }
            else if (aas.contains("integrator")) {
                check_pins(c.name, "integrator", {"in", "out"});
                const json& b = behavioral_of(aas.at("integrator"), "integrator");
                const double gain = required(b, "gain", "integrator");
                // initial: "Initial output value (at t=0)" — absent = 0, matching the SPICE
                // IC default. reference: "Setpoint subtracted from the input" — absent = 0.
                const double initial = optional_with_documented_default(b, "initial", 0.0);
                const double ref     = optional_with_documented_default(b, "reference", 0.0);
                std::optional<double> lo, hi;
                if (b.contains("outputLow"))  lo = resolved_leaf(b.at("outputLow"),  "outputLow of "  + c.name);
                if (b.contains("outputHigh")) hi = resolved_leaf(b.at("outputHigh"), "outputHigh of " + c.name);
                if (lo && hi && *lo >= *hi)
                    throw std::runtime_error(
                        "CiasCircuitConverter: integrator '" + c.name + "' clamp outputLow >= outputHigh");
                const std::string in   = node_of(c.name, "in");
                const std::string out  = node_of(c.name, "out");
                const std::string raw  = c.name + "__raw";
                const std::string vr = "V(" + raw + ")";
                body << "B" << c.name << "_i 0 " << raw << " I=" << num(gain) << "*(V(" << in << ")-("
                     << num(ref) << "))";
                if (lo || hi) {
                    // Anti-windup: bleed the integrator state back toward the active clamp.
                    // kAw is a realization constant of the ideal-block model (fast bleed), not
                    // schema data.
                    const double kAw = 1e4;
                    body << "-" << num(kAw) << "*(";
                    bool first = true;
                    if (hi) {
                        body << "(" << tern(vr + ">(" + num(*hi) + ")", vr + "-(" + num(*hi) + ")", "0") << ")";
                        first = false;
                    }
                    if (lo) {
                        if (!first) body << "+";
                        body << "(" << tern(vr + "<(" + num(*lo) + ")", vr + "-(" + num(*lo) + ")", "0") << ")";
                    }
                    body << ")";
                }
                body << "\n";
                body << "C" << c.name << "_int " << raw << " 0 1 IC=" << num(initial) << "\n";
                std::string clampExpr = vr;
                if (hi) clampExpr = tern(vr + ">(" + num(*hi) + ")", num(*hi), clampExpr);
                if (lo) clampExpr = tern(vr + "<(" + num(*lo) + ")", num(*lo), clampExpr);
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

            // Substitute expression variables (used by flux/charge):
            //   i(<designator>)  -> I(V<designator>)   (sibling source current, both dialects)
            //   bare i           -> own port current   (dialect-specific realization)
            //   bare v           -> own branch voltage
            auto subst_own_vars = [&](std::string expr,
                                      const std::string& own_current,
                                      const std::string& own_voltage) -> std::string {
                expr = subst_sibling_currents(expr);
                expr = std::regex_replace(expr, std::regex(R"(\bi\b(?!\s*\())"), own_current);
                expr = std::regex_replace(expr, std::regex(R"(\bv\b(?!\s*\())"), own_voltage);
                return expr;
            };

            if (nature == "flux" || nature == "charge") {
                check_pins(c.name, "behavioral", {"1", "2"});
                const std::string n1 = node_of(c.name, "1");
                const std::string n2 = node_of(c.name, "2");
                const std::string expr = beh.at("expression").get<std::string>();
                if (dialect == SpiceDialect::Ltspice) {
                    // Native LTspice attributes: the branch variable is 'x'
                    // (i for Flux=, v for Q=).
                    const char* ownVar = (nature == "flux") ? R"(\bi\b(?!\s*\())" : R"(\bv\b(?!\s*\())";
                    std::string lt_expr = subst_sibling_currents(expr);
                    lt_expr = std::regex_replace(lt_expr, std::regex(ownVar), "x");
                    body << (nature == "flux" ? "L" : "C") << c.name << " " << n1 << " " << n2
                         << (nature == "flux" ? " Flux=" : " Q=") << lt_expr << "\n";
                } else {
                    // ngspice >=43: B-element with ddt() + series Vsense current sensor.
                    // Sense node order is PORT-NODE FIRST (n1 -> internal): SPICE positive
                    // current flows from the + node through the source, so I(sense) is the
                    // current ENTERING terminal 1 — the same convention as the Chan branch
                    // and LTspice's x. (Reversed order sign-flips i and turns an odd Psi(i)
                    // into an energy-GENERATING inductor.)
                    // Requires: .options method=gear (stiff ODE; avoids trapezoidal ringing)
                    const std::string sense = "Vsense_" + c.name;
                    const std::string n_int = n1 + "__" + c.name;
                    const std::string subst = subst_own_vars(
                        expr, "I(" + sense + ")", "V(" + n1 + "," + n2 + ")");
                    body << sense << " " << n1 << " " << n_int << " DC 0\n";
                    body << "B" << c.name << " " << n_int << " " << n2
                         << (nature == "flux" ? " V=ddt(" : " I=ddt(") << subst << ")\n";
                }
            }
            else if (nature == "source") {
                // Fixed internal source (macromodel part). A 0 V source is an ideal ammeter:
                // sibling expressions reference its current as i(<this designator>).
                const std::string quantity = beh.at("quantity").get<std::string>();
                const json& across = beh.at("across");
                const std::string a0 = across[0].get<std::string>();
                const std::string a1 = across[1].get<std::string>();
                check_pins(c.name, "behavioral source", {a0, a1});
                const double value = resolved_leaf(beh.at("value"), "value of source " + c.name);
                if (quantity == "voltage")
                    body << "V" << c.name << " " << node_of(c.name, a0) << " "
                         << node_of(c.name, a1) << " " << num(value) << "\n";
                else if (quantity == "current")
                    body << "I" << c.name << " " << node_of(c.name, a0) << " "
                         << node_of(c.name, a1) << " " << num(value) << "\n";
                else
                    throw std::runtime_error(
                        "CiasCircuitConverter: source '" + c.name + "' has unknown quantity '" +
                        quantity + "' (expected voltage/current)");
            }
            else if (nature == "switch") {
                // Voltage-controlled hysteretic switch (SPICE S / .model SW).
                const json& across = beh.at("across");
                const json& control = beh.at("control");
                const std::string a0 = across[0].get<std::string>(), a1 = across[1].get<std::string>();
                const std::string c0 = control[0].get<std::string>(), c1 = control[1].get<std::string>();
                check_pins(c.name, "behavioral switch", {a0, a1, c0, c1});
                const double ron  = beh.at("ron").get<double>();
                const double roff = beh.at("roff").get<double>();
                const double vOn  = beh.at("vOn").get<double>();
                const double vOff = beh.at("vOff").get<double>();
                if (vOn <= vOff)
                    throw std::runtime_error(
                        "CiasCircuitConverter: switch '" + c.name + "' has vOn <= vOff — "
                        "zero/negative hysteresis switches are not supported (LTspice treats "
                        "negative Vh as a smooth transition, changing the semantics)");
                const std::string model = "SWM_" + c.name;
                body << "S" << c.name << " " << node_of(c.name, a0) << " " << node_of(c.name, a1)
                     << " " << node_of(c.name, c0) << " " << node_of(c.name, c1) << " " << model << "\n";
                if (dialect == SpiceDialect::Ltspice)
                    body << ".model " << model << " SW(Ron=" << num(ron) << " Roff=" << num(roff)
                         << " Vt=" << num((vOn + vOff) / 2.0) << " Vh=" << num((vOn - vOff) / 2.0)
                         << ")\n";
                else
                    body << ".model " << model << " SW(Ron=" << num(ron) << " Roff=" << num(roff)
                         << " Von=" << num(vOn) << " Voff=" << num(vOff) << ")\n";
            }
            else if (nature == "controlled") {
                // Controlled/dependent source: value = f(own terminals, sibling currents).
                // Emit between the 'across' pins; substitute i(p,q)->I(Vsense) (synthesising a
                // 0 V sense per distinct i()), v(p,q)->V(node,node), i(NAME)->I(VNAME);
                // pwl()->table() for LTspice.
                const json& out = beh.at("output");
                const std::string quantity = out.at("quantity").get<std::string>();
                if (quantity != "current" && quantity != "voltage")
                    throw std::runtime_error(
                        "CiasCircuitConverter: controlled source '" + c.name +
                        "' has unknown output quantity '" + quantity + "'");
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

                auto subst_two_arg = [&](const std::string& fn, bool isCurrent,
                                         std::ostringstream& senses, int& k) {
                    std::regex re("\\b" + fn + "\\s*\\(\\s*(\\w+)\\s*,\\s*(\\w+)\\s*\\)",
                                  std::regex_constants::icase);
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
                subst_two_arg("i", true, senses, k);   // i(p,q) -> I(Vsense)
                subst_two_arg("v", false, senses, k);  // v(p,q) -> V(node,node)
                expr = subst_sibling_currents(expr);   // i(NAME) -> I(VNAME)
                if (dialect == SpiceDialect::Ltspice)
                    expr = std::regex_replace(expr, std::regex(R"(\bpwl\s*\()", std::regex_constants::icase),
                                              "table(");
                body << senses.str();
                body << "B" << c.name << " " << na << " " << nb << " "
                     << (quantity == "current" ? "I=" : "V=") << expr << "\n";
            }
            else if (nature == "chan") {
                check_pins(c.name, "behavioral chan", {"primary_start", "primary_end"});
                // Chan saturable core (LTspice Hc/Bs/Br material + A/Lm/Lg/N geometry).
                const json& p = beh.at("parameters");
                const std::string ps  = node_of(c.name, "primary_start");
                const std::string pe  = node_of(c.name, "primary_end");
                if (dialect == SpiceDialect::Ltspice) {
                    // B-H params (Hc/Bs/Br) may be temperature curves -> rebuild tbl(temp,...).
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
                    // that are temperature curves are resolved at their referenceTemperature,
                    // which must be present and inside the curve's range (no silent defaults).
                    auto bh = [&](const char* key) -> double {
                        const json& v = p.at(key);
                        if (v.is_number()) return v.get<double>();
                        if (!v.contains("referenceTemperature"))
                            throw std::runtime_error(
                                "CiasCircuitConverter: chan '" + c.name + "' parameter '" + key +
                                "' is a temperature curve but has no referenceTemperature to "
                                "resolve it at");
                        const double Tref = v.at("referenceTemperature").get<double>();
                        const json& pts = v.at("perTemperature");
                        const double tFirst = pts.front().at("temperature").get<double>();
                        const double tLast  = pts.back().at("temperature").get<double>();
                        if (Tref < tFirst || Tref > tLast)
                            throw std::runtime_error(
                                "CiasCircuitConverter: chan '" + c.name + "' parameter '" + key +
                                "' referenceTemperature " + num(Tref) + " lies outside the curve "
                                "range [" + num(tFirst) + ", " + num(tLast) + "]");
                        double t0 = tFirst;
                        double v0 = pts.front().at("value").get<double>();
                        for (size_t i = 1; i < pts.size(); ++i) {
                            const double t1 = pts[i].at("temperature").get<double>();
                            const double v1 = pts[i].at("value").get<double>();
                            if (Tref <= t1) return v0 + (Tref - t0) / (t1 - t0) * (v1 - v0);
                            t0 = t1; v0 = v1;
                        }
                        return v0;  // Tref == tLast
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
            else if (nature == "frequencyResponse") {
                throw std::runtime_error(
                    "CiasCircuitConverter: behavioral nature 'frequencyResponse' (tabulated H(f)) "
                    "is not implemented yet for component '" + c.name + "'");
            }
            else {
                throw std::runtime_error(
                    "CiasCircuitConverter: behavioral component '" + c.name +
                    "' has unknown nature '" + nature +
                    "' — expected flux/charge/chan/controlled/source/switch");
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
        "resistor", "capacitor", "magnetic", "semiconductor", "varistor",
        "controller", "connector", "analog", "behavioral", "transmissionLine"};
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
        int portEndpoints = 0;
        for (const auto& ep : conn.endpoints) {
            if (ep.isPinEndpoint()) {
                if (!compNames.count(ep.component))
                    problems.push_back("connection '" + conn.name +
                                       "' references unknown component '" + ep.component + "'");
            } else if (ep.isPortEndpoint()) {
                ++portEndpoints;
                if (!portNames.count(ep.port))
                    problems.push_back("connection '" + conn.name +
                                       "' references unknown port '" + ep.port + "'");
            } else {
                problems.push_back("connection '" + conn.name + "' has a malformed endpoint");
            }
        }
        if (portEndpoints > 1)
            problems.push_back("connection '" + conn.name + "' is exposed at " +
                               std::to_string(portEndpoints) +
                               " ports (max 1 — tie ports with an explicit 0 V source)");
    }
    return problems;
}

} // namespace CIAS
