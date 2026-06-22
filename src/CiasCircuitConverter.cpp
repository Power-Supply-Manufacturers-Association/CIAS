#include "CiasCircuitConverter.hpp"
#include <sstream>
#include <map>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <optional>

namespace CIAS {

namespace {

std::string num(double v) {
    std::ostringstream os;
    os.precision(10);
    os << v;
    return os.str();
}

// net node name for a connection: a net exposed at a brick port takes the port name, else the
// connection's own (brick-local) name.
std::string net_node_name(const Connection& conn) {
    for (const auto& ep : conn.endpoints)
        if (ep.isPortEndpoint())
            return ep.port;
    return conn.name;
}

// nominal value at data[keys...] -> a number; throws if absent.
double nominal_at(const json& data, std::initializer_list<const char*> keys, const std::string& what) {
    const json* cur = &data;
    for (const char* k : keys) {
        if (!cur->is_object() || !cur->contains(k))
            throw std::runtime_error("CIAS->ngspice: missing '" + std::string(k) + "' for " + what);
        cur = &cur->at(k);
    }
    if (cur->is_number()) return cur->get<double>();
    if (cur->is_object() && cur->contains("nominal") && cur->at("nominal").is_number())
        return cur->at("nominal").get<double>();
    throw std::runtime_error("CIAS->ngspice: non-numeric value for " + what);
}

} // namespace

std::string CiasToNgspiceConverter::to_cards(const CiasCircuit& circuit) const {
    // (component, pin) -> node
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
            throw std::runtime_error("CIAS->ngspice: pin '" + pin + "' of component '" + comp +
                                     "' in brick '" + circuit.name + "' is not wired");
        return it->second;
    };

    std::ostringstream body;
    for (const auto& c : circuit.components) {
        const json& d = c.data;
        if (!d.is_object())
            throw std::runtime_error("CIAS->ngspice: component '" + c.name + "' has no PEAS data");

        if (d.contains("resistor")) {
            double r = nominal_at(d, {"resistor", "manufacturerInfo", "datasheetInfo", "electrical",
                                      "resistance"}, "resistor " + c.name);
            body << "R" << c.name << " " << node_of(c.name, "1") << " " << node_of(c.name, "2")
                 << " " << num(r) << "\n";
        }
        else if (d.contains("capacitor")) {
            double cap = nominal_at(d, {"capacitor", "manufacturerInfo", "datasheetInfo",
                                        "electrical", "capacitance"}, "capacitor " + c.name);
            body << "C" << c.name << " " << node_of(c.name, "1") << " " << node_of(c.name, "2")
                 << " " << num(cap) << "\n";
        }
        else if (d.contains("semiconductor")) {
            const json& semi = d.at("semiconductor");
            if (semi.contains("mosfet")) {
                double ron = nominal_at(d, {"semiconductor", "mosfet", "manufacturerInfo",
                                            "datasheetInfo", "electrical", "onResistance"},
                                        "mosfet " + c.name);
                double vth = nominal_at(d, {"semiconductor", "mosfet", "manufacturerInfo",
                                            "datasheetInfo", "electrical", "gateThresholdVoltage"},
                                        "mosfet " + c.name);
                const std::string model = "SW_" + c.name;
                // vc-switch: Sxxx n+ n- nc+ nc- model. Control is gate-to-GROUND (like MKF's
                // ground-referenced pwm_ctrl), independent of the power path — so a HIGH-SIDE switch
                // (buck etc.) works too. For a low-side switch source==0, so this is identical.
                body << "S" << c.name << " " << node_of(c.name, "drain") << " "
                     << node_of(c.name, "source") << " " << node_of(c.name, "gate") << " "
                     << "0" << " " << model << "\n";
                // Hysteresis / off-resistance pinned to MKF's ideal switch (.model SW1 SW
                // VT=2.5 VH=0.5 RON=0.01 ROFF=1e6) so an ideal Kirchhoff deck matches MKF's.
                body << ".model " << model << " SW(Vt=" << num(vth) << " Vh=0.5 Ron=" << num(ron)
                     << " Roff=1e6)\n";
            }
            else if (semi.contains("diode")) {
                const json& el = d.at("semiconductor").at("diode").at("manufacturerInfo")
                                  .at("datasheetInfo").at("electrical");
                auto optnum = [&](const char* key) -> std::optional<double> {
                    if (!el.contains(key)) return std::nullopt;
                    const json& v = el.at(key);
                    if (v.is_number()) return v.get<double>();
                    if (v.is_object() && v.contains("nominal") && v.at("nominal").is_number())
                        return v.at("nominal").get<double>();
                    return std::nullopt;
                };
                const double Vt = 0.025852;
                const double vf = optnum("forwardVoltage").value();        // SAS guarantees presence
                const double i0 = optnum("forwardVoltageAt").value_or(1.0); // current Vf is rated at
                // Saturation current so the forward drop ~= vf at i0 (single-exponential, N=1). For the
                // ideal diode (vf=0.8334, i0=1) this is IS=1e-14 (= MKF DIDEAL); a real part uses its
                // datasheet Vf@I plus the parasitics below.
                const double isat = i0 * std::exp(-vf / Vt);
                const std::string model = "D_" + c.name;
                body << "D" << c.name << " " << node_of(c.name, "anode") << " "
                     << node_of(c.name, "cathode") << " " << model << "\n";
                std::ostringstream m;
                m << ".model " << model << " D(IS=" << num(isat) << " N=1 RS=1e-6";
                if (auto cj  = optnum("junctionCapacitance")) m << " CJO=" << num(*cj);  // junction cap
                if (auto trr = optnum("reverseRecoveryTime"))  m << " TT="  << num(*trr); // ~transit time
                if (auto bv  = optnum("breakdownVoltage"))     m << " BV="  << num(*bv);
                else if (auto rv = optnum("reverseVoltage"))   m << " BV="  << num(*rv);
                m << ")";
                body << m.str() << "\n";
            }
            else {
                throw std::runtime_error("CIAS->ngspice: semiconductor '" + c.name +
                                         "' is neither mosfet nor diode (igbt/bjt = Phase 3)");
            }
        }
        else if (d.contains("magnetic")) {
          const json& mag = d.at("magnetic");
          // MKF_MODEL fidelity: a designed magnetic carries its MKF-exported ngspice subcircuit
          // (real winding Rdc + AC-resistance ladder + magnetizing L + leakage coupling, all from
          // CircuitSimulatorExporter — the single source of truth for magnetics math). Inline the
          // self-contained .subckt (its .params are locally scoped, so multiple magnetics don't
          // collide) and instantiate it, mapping winding terminals to the per-winding ports
          // P<i>+/P<i>- (primary = winding 1, secondaryN = winding N+1).
          if (mag.contains("modelOutputs") && mag.at("modelOutputs").contains("spiceSubcircuit")) {
            // The .subckt DEFINITION is hoisted to the deck top level by the assembler (it can't live
            // in a stage body — nested .subckt/.param/.ends get mangled). Here we emit ONLY the X
            // instance that references it, wiring winding terminals -> per-winding ports P<i>+/-.
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
            // One multi-winding component -> L per winding + pairwise K coupling.
            const double lp = nominal_at(d, {"inputs", "designRequirements", "magnetizingInductance"},
                                         "magnetic " + c.name);
            std::vector<double> ratios;
            if (const json* tr = (d.at("inputs").at("designRequirements").contains("turnsRatios")
                                  ? &d.at("inputs").at("designRequirements").at("turnsRatios") : nullptr)) {
                for (const auto& e : *tr)
                    ratios.push_back(e.is_object() && e.contains("nominal") ? e.at("nominal").get<double>()
                                                                           : e.get<double>());
            }
            // Winding coupling K. MODELING lives in the magnetic family (MAS::make_magnetic_atom /
            // compute_coupling, decisions 2/5): MAS decides K (from a specified leakage inductance, else
            // the 0.9999 ideal default) and carries it in the atom; this emitter only renders it. The
            // 0.9999 fallback here is just for atoms produced before this field existed. (Why never 1.0:
            // perfect coupling makes the coupled-L matrix singular, det = Lp*Ls*(1-K^2) = 0, which blows
            // up ngspice whenever a transformer is in series with another inductor — every bridge /
            // resonant topology. Real designed magnetics use the MKF_MODEL subcircuit branch above, whose
            // coupling comes from the actual leakage.)
            const json& mdr = d.at("inputs").at("designRequirements");
            const double k = mdr.contains("coupling") ? mdr.at("coupling").get<double>() : 0.9999;
            std::vector<std::string> indNames;

            const std::string lpri = "L" + c.name + "_pri";
            body << lpri << " " << node_of(c.name, "primary_start") << " "
                 << node_of(c.name, "primary_end") << " " << num(lp) << "\n";
            indNames.push_back(lpri);

            for (size_t i = 0; i < ratios.size(); ++i) {
                if (ratios[i] == 0.0) throw std::runtime_error("CIAS->ngspice: zero turns ratio");
                const double ls = lp / (ratios[i] * ratios[i]);
                const std::string idx = std::to_string(i + 1);
                const std::string lsec = "L" + c.name + "_sec" + idx;
                body << lsec << " " << node_of(c.name, "secondary" + idx + "_start") << " "
                     << node_of(c.name, "secondary" + idx + "_end") << " " << num(ls) << "\n";
                indNames.push_back(lsec);
            }
            // pairwise coupling
            for (size_t i = 0; i < indNames.size(); ++i)
                for (size_t j = i + 1; j < indNames.size(); ++j)
                    body << "K" << c.name << "_" << i << j << " " << indNames[i] << " "
                         << indNames[j] << " " << num(k) << "\n";
          }
        }
        else {
            throw std::runtime_error("CIAS->ngspice: component '" + c.name +
                                     "' has an unknown PEAS discriminator");
        }
    }
    return body.str();
}

std::string CiasToNgspiceConverter::to_subckt(const CiasCircuit& circuit) const {
    std::ostringstream out;
    out << ".subckt " << circuit.name;
    for (const auto& p : circuit.ports) out << " " << p.name;
    out << "\n";
    out << to_cards(circuit);
    out << ".ends " << circuit.name << "\n";
    return out.str();
}

std::string CiasToNgspiceConverter::to_subckt_json(const json& ciasJson) const {
    return to_subckt(CiasCircuit::from_json(ciasJson));
}

} // namespace CIAS
