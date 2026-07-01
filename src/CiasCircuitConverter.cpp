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

    // Does this circuit contain any K-coupled (multi-winding) magnetic? Only then does a
    // mutual-inductance matrix exist to be ill-conditioned, so only then do inductors get
    // the tiny native Rser regularizer. An ISOLATED ideal inductor must stay ideal: adding
    // even 1e-9 Ω shifts LTspice's low-frequency |Z| (a sub-milliohm ideal-inductor quirk),
    // which would make single-inductor parts diverge from the vendor model.
    bool circuitHasCoupling = false;
    for (const auto& c : circuit.components) {
        const json& d = c.data;
        if (d.is_object() && d.contains("magnetic")
            && d.contains("inputs") && d.at("inputs").is_object()
            && d.at("inputs").contains("designRequirements")
            && d.at("inputs").at("designRequirements").contains("turnsRatios")
            && d.at("inputs").at("designRequirements").at("turnsRatios").is_array()
            && !d.at("inputs").at("designRequirements").at("turnsRatios").empty()) {
            circuitHasCoupling = true;
            break;
        }
    }

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
            if (!dev.contains("spiceModel")) {
                // NO spiceModel → build the SPICE model from the device's electrical block (the ron/
                // vth/Vf SAS carries for an ideal OR a requirements-realized part), NOT from a per-part
                // SPICE .model card. The whole point of CIAS is to render circuits that need not carry
                // vendor models: an ideal switch emits MKF's canonical S-model; a realized part emits
                // the SAME S/D template parameterized by its resolved on-resistance / forward voltage.
                // Only when a field is truly absent do we fall back to the ideal constant (a switch's
                // canonical model is a realization constant, not fabricated datasheet data). A REAL
                // vendor part carries a spiceModel and takes the faithful-.model path below.
                const json* elec = nullptr;
                if (dev.contains("manufacturerInfo") && dev.at("manufacturerInfo").is_object()
                    && dev.at("manufacturerInfo").contains("datasheetInfo")
                    && dev.at("manufacturerInfo").at("datasheetInfo").is_object()
                    && dev.at("manufacturerInfo").at("datasheetInfo").contains("electrical"))
                    elec = &dev.at("manufacturerInfo").at("datasheetInfo").at("electrical");
                auto optnum = [&](const char* key) -> std::optional<double> {
                    if (!elec || !elec->contains(key)) return std::nullopt;
                    try { return PEAS::resolve_dimensional_values(elec->at(key)); }
                    catch (const std::exception&) { return std::nullopt; }
                };
                if (devKey == "mosfet") {
                    check_pins(c.name, "mosfet", {"drain", "gate", "source"});
                    // Resolved on-resistance / gate threshold, else MKF's ideal switch constants.
                    const double ron = optnum("onResistance").value_or(0.01);
                    const double vth = optnum("gateThresholdVoltage").value_or(2.5);
                    const std::string model = "SW_" + c.name;
                    // vc-switch: Sxxx n+ n- nc+ nc- model. Control is gate-to-GROUND (MKF's
                    // ground-referenced pwm_ctrl), independent of the power path — so a HIGH-SIDE
                    // switch (buck etc.) works too; for a low-side switch source==0, so it's identical.
                    body << "S" << c.name << " " << node_of(c.name, "drain") << " "
                         << node_of(c.name, "source") << " " << node_of(c.name, "gate") << " 0 "
                         << model << "\n";
                    body << ".model " << model << " SW(Vt=" << num(vth) << " Vh=0.5 Ron="
                         << num(ron) << " Roff=1e6)\n";
                } else if (devKey == "diode") {
                    check_pins(c.name, "diode", {"anode", "cathode"});
                    const std::string model = "D_" + c.name;
                    body << "D" << c.name << " " << node_of(c.name, "anode") << " "
                         << node_of(c.name, "cathode") << " " << model << "\n";
                    // Saturation current so the forward drop ≈ Vf at its rated current (single
                    // exponential, N=1): IS = i0·exp(-Vf/Vt). Ideal diode (Vf=0.8334 @ 1 A) ⇒
                    // IS=1e-14 = MKF DIDEAL; a realized part uses its resolved Vf plus any parasitics.
                    // No forwardVoltage at all ⇒ the canonical DIDEAL constant directly.
                    if (auto vf = optnum("forwardVoltage")) {
                        const double Vt = 0.025852;   // ngspice 27 °C thermal voltage
                        const double i0 = optnum("forwardVoltageAt").value_or(1.0);
                        const double isat = i0 * std::exp(-*vf / Vt);
                        std::ostringstream m;
                        m << ".model " << model << " D(IS=" << num(isat) << " N=1 RS=1e-6";
                        if (auto cj  = optnum("junctionCapacitance")) m << " CJO=" << num(*cj);
                        if (auto trr = optnum("reverseRecoveryTime"))  m << " TT="  << num(*trr);
                        if (auto bv  = optnum("breakdownVoltage"))     m << " BV="  << num(*bv);
                        else if (auto rv = optnum("reverseVoltage"))   m << " BV="  << num(*rv);
                        m << ")";
                        body << m.str() << "\n";
                    } else {
                        body << ".model " << model << " D(IS=1e-14 N=1 RS=1e-6)\n";
                    }
                } else {
                    throw std::runtime_error(
                        "CiasCircuitConverter: model-less emission for '" + c.name + "' (" + devKey +
                        ") is not supported — only mosfet/diode have an ideal realization; a bjt/igbt "
                        "must carry a spiceModel");
                }
            } else {
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
                // A pure-ideal inductor makes LTspice's mutual-inductance matrix
                // ill-conditioned (singular at k=1, near-singular otherwise) — a small
                // uncoupled winding beside a k=1 pair yields garbage |Z| (100s of %). The
                // real winding resistance normally regularizes it, but here DCR lives in a
                // sibling R (for losslessness/composability), leaving the L ideal. Emit a
                // negligible native Rser (1e-9 Ω ≪ any real DCR) purely as a numerical
                // regularizer. LTspice-only: ngspice has no L Rser= (and caps k already).
                const std::string lReg =
                    (dialect == SpiceDialect::Ltspice && circuitHasCoupling) ? " Rser=1e-9" : "";
                std::vector<std::string> indNames;
                const std::string lpri = "L" + c.name + "_pri";
                body << lpri << " " << node_of(c.name, "primary_start") << " "
                     << node_of(c.name, "primary_end") << " " << num(lp) << lReg << "\n";
                indNames.push_back(lpri);

                for (size_t i = 0; i < ratios.size(); ++i) {
                    if (ratios[i] == 0.0)
                        throw std::runtime_error(
                            "CiasCircuitConverter: zero turns ratio in magnetic " + c.name);
                    const double ls = lp / (ratios[i] * ratios[i]);
                    const std::string idx = std::to_string(i + 1);
                    const std::string lsec = "L" + c.name + "_sec" + idx;
                    body << lsec << " " << node_of(c.name, "secondary" + idx + "_start") << " "
                         << node_of(c.name, "secondary" + idx + "_end") << " " << num(ls) << lReg << "\n";
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
                        // Mutual-coupling name keeps the established "<name>_<ij>" deck format (winding
                        // indices concatenated), e.g. KT1_01 for the primary↔secondary1 pair — the
                        // stable contract Kirchhoff's decks and equivalence tests are written against.
                        body << "K" << c.name << "_" << i << j << " " << indNames[i] << " "
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
            else if (aas.contains("sampleHold")) {
                // Ideal S/H (AAS sampleHold behavioral, PEAS-RFC 0001 §5.2/§7): one canonical
                // template per mode — ideal switch + 1 nF hold capacitor + unity E-buffer.
                // trackWhileActive closes the switch while the trigger condition holds;
                // sampleOnEdge derives a narrow (~7 ns) sampling pulse from an edge-detect RC
                // (tau 10 ns) so the 1 ns switch+cap tracking constant settles within it.
                check_pins(c.name, "sampleHold", {"in", "trg", "out"});
                const json& b = behavioral_of(aas.at("sampleHold"), "sampleHold");
                auto required_str = [&](const char* key) -> std::string {
                    if (!b.contains(key) || !b.at(key).is_string())
                        throw std::runtime_error(
                            "CiasCircuitConverter: analog sampleHold '" + c.name +
                            "' behavioral block is missing required '" + std::string(key) + "'");
                    return b.at(key).get<std::string>();
                };
                const std::string mode     = required_str("mode");
                const std::string polarity = required_str("polarity");
                const double thr = required(b, "threshold", "sampleHold");
                if (polarity != "activeHigh" && polarity != "activeLow")
                    throw std::runtime_error(
                        "CiasCircuitConverter: sampleHold '" + c.name + "' has unknown polarity '" +
                        polarity + "' (expected activeHigh/activeLow)");
                const std::string in   = node_of(c.name, "in");
                const std::string trg  = node_of(c.name, "trg");
                const std::string out  = node_of(c.name, "out");
                const std::string hold = c.name + "__hold";
                const std::string ctl  = c.name + "__ctl";
                const std::string model = "SHM_" + c.name;
                // Trigger condition, polarity-normalised to "active = 1".
                const std::string actCond = "V(" + trg + ")" + (polarity == "activeHigh" ? ">" : "<")
                                          + "(" + num(thr) + ")";
                if (mode == "trackWhileActive") {
                    body << "B" << c.name << "_ctl " << ctl << " 0 V=" << tern(actCond, "1", "0") << "\n";
                } else if (mode == "sampleOnEdge") {
                    const std::string lvl  = c.name + "__lvl";
                    const std::string lvld = c.name + "__lvld";
                    body << "B" << c.name << "_lvl " << lvl << " 0 V=" << tern(actCond, "1", "0") << "\n";
                    body << "R" << c.name << "_ed " << lvl << " " << lvld << " 10\n";
                    body << "C" << c.name << "_ed " << lvld << " 0 1n\n";
                    body << "B" << c.name << "_ctl " << ctl << " 0 V="
                         << tern("V(" + lvl + ")-V(" + lvld + ")>0.5", "1", "0") << "\n";
                } else {
                    throw std::runtime_error(
                        "CiasCircuitConverter: sampleHold '" + c.name + "' has unknown mode '" +
                        mode + "' (expected trackWhileActive/sampleOnEdge)");
                }
                body << "S" << c.name << " " << in << " " << hold << " " << ctl << " 0 " << model << "\n";
                body << "C" << c.name << "_hold " << hold << " 0 1n\n";
                body << "E" << c.name << " " << out << " 0 " << hold << " 0 1\n";
                // Vt/Vh (not Von/Voff): ngspice-42 ignores Von/Voff on SW models; Vt/Vh is the
                // form both dialects accept (same precedent as the comparator emission).
                body << ".model " << model << " SW(Vt=0.5 Vh=0.2 Ron=1 Roff=1e9)\n";
            }
            else {
                throw std::runtime_error(
                    "CiasCircuitConverter: analog '" + c.name +
                    "' block type not supported (comparator/multiplier/summer/integrator/"
                    "sampleHold only)");
            }
        }
        else if (d.contains("timeBase")) {
            // TDAS time-base atoms (PEAS-RFC 0001 §7): oscillator / timer / latch. Realization
            // requires the family's `behavioral` block — a datasheet-only orderable part must
            // NOT silently become a fabricated ideal block (no-fallbacks rule). Each canonical
            // subcircuit below is ONE template; only parameter values vary per call.
            const json& tb = d.at("timeBase");
            std::string famKey;
            for (const char* k : {"oscillator", "timer", "latch"})
                if (tb.contains(k)) { famKey = k; break; }
            if (famKey.empty())
                throw std::runtime_error(
                    "CiasCircuitConverter: timeBase '" + c.name +
                    "' has no oscillator/timer/latch family");
            const json& fam = tb.at(famKey);
            if (!fam.contains("behavioral") || !fam.at("behavioral").is_object())
                throw std::runtime_error(
                    "CiasCircuitConverter: timeBase " + famKey + " '" + c.name +
                    "' has no behavioral block — cannot realize a datasheet-only part as an "
                    "ideal element");
            const json& b = fam.at("behavioral");
            auto required = [&](const char* key) -> double {
                if (!b.contains(key))
                    throw std::runtime_error(
                        "CiasCircuitConverter: timeBase " + famKey + " '" + c.name +
                        "' behavioral block is missing required '" + key + "'");
                return resolved_leaf(b.at(key), std::string(key) + " of " + c.name);
            };
            auto required_str = [&](const char* key) -> std::string {
                if (!b.contains(key) || !b.at(key).is_string())
                    throw std::runtime_error(
                        "CiasCircuitConverter: timeBase " + famKey + " '" + c.name +
                        "' behavioral block is missing required '" + std::string(key) + "'");
                return b.at(key).get<std::string>();
            };
            const double kTwoPi = 2.0 * std::acos(-1.0);

            if (famKey == "oscillator") {
                const bool vco = b.contains("frequencyControl");
                {
                    std::set<std::string> allowed = {"outPlus", "outMinus"};
                    if (vco) { allowed.insert("ctrlPlus"); allowed.insert("ctrlMinus"); }
                    check_pins(c.name, "oscillator", allowed);
                }
                const std::string shape = required_str("shape");
                const double f0  = required("frequency");
                const double amp = required("amplitude");
                const double off = required("offset");
                if (f0 <= 0.0)
                    throw std::runtime_error(
                        "CiasCircuitConverter: oscillator '" + c.name + "' has non-positive frequency");
                if (amp <= 0.0)
                    throw std::runtime_error(
                        "CiasCircuitConverter: oscillator '" + c.name + "' has non-positive amplitude");
                if (shape != "sawtooth" && shape != "triangle" && shape != "square" && shape != "sine")
                    throw std::runtime_error(
                        "CiasCircuitConverter: oscillator '" + c.name + "' has unknown shape '" +
                        shape + "' (expected sawtooth/triangle/square/sine)");
                if (shape == "square" && !b.contains("dutyCycle"))
                    throw std::runtime_error(
                        "CiasCircuitConverter: oscillator '" + c.name +
                        "' behavioral block is missing required 'dutyCycle' (shape=square)");
                if (shape != "square" && b.contains("dutyCycle"))
                    throw std::runtime_error(
                        "CiasCircuitConverter: oscillator '" + c.name +
                        "' has dutyCycle but shape is not 'square'");
                double duty = 0.0;
                if (shape == "square") {
                    duty = resolved_leaf(b.at("dutyCycle"), "dutyCycle of " + c.name);
                    if (duty <= 0.0 || duty >= 1.0)
                        throw std::runtime_error(
                            "CiasCircuitConverter: oscillator '" + c.name +
                            "' dutyCycle must lie in (0, 1)");
                }
                // phase: absent = the waveform origin (a documented convention of the schema,
                // not a silent numeric default).
                const double phase = b.contains("phase")
                    ? resolved_leaf(b.at("phase"), "phase of " + c.name) : 0.0;
                const std::string outP = node_of(c.name, "outPlus");
                const std::string outM = node_of(c.name, "outMinus");

                if (!vco) {
                    // Fixed frequency -> native independent source. Span convention:
                    // offset..offset+amplitude (saw/tri/square), offset±amplitude (sine);
                    // phase becomes a time delay td = phase/(2*pi*f0). eps = period/1000 is
                    // the ideal-edge realization constant of the template.
                    const double period = 1.0 / f0;
                    const double td  = phase / (kTwoPi * f0);
                    const double eps = period * 1e-3;
                    if (shape == "sine") {
                        body << "V" << c.name << " " << outP << " " << outM << " "
                             << (dialect == SpiceDialect::Ltspice ? "SINE(" : "SIN(")
                             << num(off) << " " << num(amp) << " " << num(f0) << " "
                             << num(td) << ")\n";
                    } else {
                        double tr, tf, pw;
                        if (shape == "sawtooth")      { tr = period - eps; tf = eps; pw = 0.0; }
                        else if (shape == "triangle") { tr = period / 2.0; tf = period / 2.0; pw = 0.0; }
                        else /* square */             { tr = eps; tf = eps; pw = duty * period; }
                        body << "V" << c.name << " " << outP << " " << outM << " PULSE("
                             << num(off) << " " << num(off + amp) << " " << num(td) << " "
                             << num(tr) << " " << num(tf) << " " << num(pw) << " "
                             << num(period) << ")\n";
                    }
                } else {
                    // VCO -> canonical phase-accumulator template: behavioral current source
                    // charging a 1 F capacitor with I = f(t) = max(0, f0 + gain*v(ctrl)) (the
                    // clamp: negative instantaneous frequency is meaningless), so V(ph) counts
                    // elapsed CYCLES; the shaping B-source wraps it with floor(). Initial
                    // phase enters as the capacitor IC in cycles (phase/(2*pi)).
                    const json& fc = b.at("frequencyControl");
                    if (!fc.contains("gain"))
                        throw std::runtime_error(
                            "CiasCircuitConverter: oscillator '" + c.name +
                            "' frequencyControl is missing required 'gain'");
                    const double gain = resolved_leaf(fc.at("gain"), "frequencyControl.gain of " + c.name);
                    const std::string cp = node_of(c.name, "ctrlPlus");
                    const std::string cn = node_of(c.name, "ctrlMinus");
                    const std::string ph = c.name + "__ph";
                    const std::string wr = "(V(" + ph + ")-floor(V(" + ph + ")))";
                    body << "B" << c.name << "_f 0 " << ph << " I=max(0,(" << num(f0) << ")+("
                         << num(gain) << ")*V(" << cp << "," << cn << "))\n";
                    body << "C" << c.name << "_ph " << ph << " 0 1 IC=" << num(phase / kTwoPi) << "\n";
                    std::string expr;
                    if (shape == "sawtooth")
                        expr = "(" + num(off) + ")+(" + num(amp) + ")*" + wr;
                    else if (shape == "triangle")
                        expr = "(" + num(off) + ")+(" + num(amp) + ")*(1-2*abs(" + wr + "-0.5))";
                    else if (shape == "square")
                        expr = tern(wr + "<(" + num(duty) + ")", num(off + amp), num(off));
                    else /* sine */
                        expr = "(" + num(off) + ")+(" + num(amp) + ")*sin(2*pi*" + wr + ")";
                    body << "B" << c.name << " " << outP << " " << outM << " V=" << expr << "\n";
                }
            }
            else if (famKey == "latch") {
                // Canonical self-holding template: a B-source computes the next state from
                // set/reset and the 1 ns-RC-delayed state node itself; dominance decides the
                // ternary NESTING order (the dominant input's check comes first). The output
                // B-source maps state 0/1 linearly onto outputLow/outputHigh.
                check_pins(c.name, "latch", {"setPlus", "setMinus", "resetPlus", "resetMinus",
                                             "outPlus", "outMinus"});
                const double sThr = required("setThreshold");
                const double rThr = required("resetThreshold");
                const double hi   = required("outputHigh");
                const double lo   = required("outputLow");
                const std::string dom = required_str("dominance");
                if (dom != "set" && dom != "reset")
                    throw std::runtime_error(
                        "CiasCircuitConverter: latch '" + c.name + "' has unknown dominance '" +
                        dom + "' (expected set/reset)");
                const std::string nd = c.name + "__d";
                const std::string nq = c.name + "__q";
                const std::string setCond = "V(" + node_of(c.name, "setPlus") + ","
                                          + node_of(c.name, "setMinus") + ")>(" + num(sThr) + ")";
                const std::string rstCond = "V(" + node_of(c.name, "resetPlus") + ","
                                          + node_of(c.name, "resetMinus") + ")>(" + num(rThr) + ")";
                const std::string hold = "V(" + nq + ")";
                const std::string stExpr = (dom == "reset")
                    ? tern(rstCond, "0", tern(setCond, "1", hold))    // reset checked FIRST
                    : tern(setCond, "1", tern(rstCond, "0", hold));
                body << "B" << c.name << "_st " << nd << " 0 V=" << stExpr << "\n";
                body << "R" << c.name << "_st " << nd << " " << nq << " 1\n";
                body << "C" << c.name << "_st " << nq << " 0 1n\n";
                body << "B" << c.name << " " << node_of(c.name, "outPlus") << " "
                     << node_of(c.name, "outMinus") << " V=(" << num(lo) << ")+((" << num(hi)
                     << ")-(" << num(lo) << "))*V(" << nq << ")\n";
            }
            else { // famKey == "timer"
                const std::string mode = required_str("mode");
                const double hi = required("outputHigh");
                const double lo = required("outputLow");
                if (mode == "astable") {
                    // Free-running rectangular output -> native PULSE between outputLow and
                    // outputHigh (eps = period/1000, the same ideal-edge constant).
                    check_pins(c.name, "timer", {"outPlus", "outMinus"});
                    const double period = required("period");
                    const double duty   = required("dutyCycle");
                    if (period <= 0.0)
                        throw std::runtime_error(
                            "CiasCircuitConverter: timer '" + c.name + "' has non-positive period");
                    if (duty <= 0.0 || duty >= 1.0)
                        throw std::runtime_error(
                            "CiasCircuitConverter: timer '" + c.name +
                            "' dutyCycle must lie in (0, 1)");
                    const double eps = period * 1e-3;
                    body << "V" << c.name << " " << node_of(c.name, "outPlus") << " "
                         << node_of(c.name, "outMinus") << " PULSE(" << num(lo) << " " << num(hi)
                         << " 0 " << num(eps) << " " << num(eps) << " " << num(duty * period)
                         << " " << num(period) << ")\n";
                } else if (mode == "monostable") {
                    // Canonical one-shot template: (1) trigger level B-source, polarity-
                    // normalised to "active = 1"; (2) edge detector = 10 ns RC delay of the
                    // level, edge = V(lvl)-V(lvld)>0.5 (~7 ns wide, long enough to settle the
                    // 1 ns state RC); (3) pulse state q, self-holding like the latch; (4) time
                    // ramp = 1 nA into 1 nF (slope 1 V/s, so V(tr) is elapsed SECONDS) with a
                    // discharge switch, compared against onTime to end the pulse.
                    // retriggerable=true: an edge always (re)sets q AND discharges the ramp;
                    // retriggerable=false: edges are examined only in the idle branch and the
                    // ramp discharges only while idle — mid-pulse triggers are ignored.
                    check_pins(c.name, "timer", {"trgPlus", "trgMinus", "outPlus", "outMinus"});
                    const double thr    = required("threshold");
                    const double onTime = required("onTime");
                    const std::string polarity = required_str("polarity");
                    if (onTime <= 0.0)
                        throw std::runtime_error(
                            "CiasCircuitConverter: timer '" + c.name + "' has non-positive onTime");
                    if (polarity != "risingEdge" && polarity != "fallingEdge")
                        throw std::runtime_error(
                            "CiasCircuitConverter: timer '" + c.name + "' has unknown polarity '" +
                            polarity + "' (expected risingEdge/fallingEdge)");
                    if (!b.contains("retriggerable") || !b.at("retriggerable").is_boolean())
                        throw std::runtime_error(
                            "CiasCircuitConverter: timeBase timer '" + c.name +
                            "' behavioral block is missing required 'retriggerable'");
                    const bool retrig = b.at("retriggerable").get<bool>();
                    const std::string lvl  = c.name + "__lvl";
                    const std::string lvld = c.name + "__lvld";
                    const std::string nd   = c.name + "__d";
                    const std::string nq   = c.name + "__q";
                    const std::string tr   = c.name + "__tr";
                    const std::string rst  = c.name + "__rst";
                    const std::string model = "TMR_" + c.name;
                    const std::string lvlCond = "V(" + node_of(c.name, "trgPlus") + ","
                                              + node_of(c.name, "trgMinus") + ")"
                                              + (polarity == "risingEdge" ? ">" : "<")
                                              + "(" + num(thr) + ")";
                    const std::string edge   = "V(" + lvl + ")-V(" + lvld + ")>0.5";
                    const std::string ended  = "V(" + tr + ")>=(" + num(onTime) + ")";
                    const std::string active = "V(" + nq + ")>0.5";
                    const std::string idle   = "V(" + nq + ")<0.5";
                    const std::string qExpr = retrig
                        ? tern(edge, "1", tern(active, tern(ended, "0", "1"), "0"))
                        : tern(active, tern(ended, "0", "1"), tern(edge, "1", "0"));
                    const std::string rstExpr = retrig
                        ? tern(edge, "1", tern(idle, "1", "0"))
                        : tern(idle, "1", "0");
                    body << "B" << c.name << "_lvl " << lvl << " 0 V=" << tern(lvlCond, "1", "0") << "\n";
                    body << "R" << c.name << "_ed " << lvl << " " << lvld << " 10\n";
                    body << "C" << c.name << "_ed " << lvld << " 0 1n\n";
                    body << "B" << c.name << "_st " << nd << " 0 V=" << qExpr << "\n";
                    body << "R" << c.name << "_st " << nd << " " << nq << " 1\n";
                    body << "C" << c.name << "_st " << nq << " 0 1n\n";
                    body << "B" << c.name << "_chg 0 " << tr << " I=1e-9\n";
                    body << "C" << c.name << "_tr " << tr << " 0 1n\n";
                    body << "B" << c.name << "_rst " << rst << " 0 V=" << rstExpr << "\n";
                    body << "S" << c.name << "_rst " << tr << " 0 " << rst << " 0 " << model << "\n";
                    // Vt/Vh, not Von/Voff: ngspice-42 ignores Von/Voff on SW models (comparator
                    // precedent — the same card works in both dialects).
                    body << ".model " << model << " SW(Vt=0.5 Vh=0.2 Ron=1 Roff=1e9)\n";
                    body << "B" << c.name << " " << node_of(c.name, "outPlus") << " "
                         << node_of(c.name, "outMinus") << " V=(" << num(lo) << ")+((" << num(hi)
                         << ")-(" << num(lo) << "))*V(" << nq << ")\n";
                } else {
                    throw std::runtime_error(
                        "CiasCircuitConverter: timer '" + c.name + "' has unknown mode '" + mode +
                        "' (expected monostable/astable)");
                }
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
                "semiconductor/analog/timeBase/behavioral");
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
        "controller", "connector", "analog", "timeBase", "behavioral", "transmissionLine"};
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
