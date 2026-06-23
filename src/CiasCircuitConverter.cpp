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
        else if (d.contains("analog")) {
            // Analog block (AAS). The portable, simulator-neutral primitives the CIAS netlist carries for
            // CONTROL: a comparator (and, later, op-amp / analog switch). Each backend realises the same
            // abstract block its own way — here ngspice; an LTspice / PLECS / Modelica backend maps the
            // identical CIAS comparator to its own comparator block. ALL behavioural parameters
            // (output rails, threshold, hysteresis) come from the AAS data — no behaviour is invented
            // here, so the realisation is faithful and the same numbers reach every backend.
            const json& aas = d.at("analog");
            if (aas.contains("comparator")) {
                const json& cmp = aas.at("comparator");
                // Behavioural params come from the AAS comparator's agnostic `behavioral` block
                // (outputHigh / outputLow / threshold / hysteresis), NOT the datasheet `electrical`.
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
                const double thr   = opt("threshold", 0.0);     // trip on V(inP)-V(inN) > thr [V]
                const double hyst  = opt("hysteresis", 0.0);    // half-width of the hysteresis band [V]
                const std::string inP = node_of(c.name, "inPlus");
                const std::string inN = node_of(c.name, "inMinus");
                const std::string out = node_of(c.name, "out");
                // Realise as a voltage-controlled SWITCH with NATIVE hysteresis (model Vt/Vh) rather than
                // a behavioural B-source: no algebraic loop, and the controlled switch is a far more
                // portable SPICE primitive (the same S/W model used for the power switches). The switch
                // ties `out` to a vHigh rail when V(inP)-V(inN) > thr (±hyst); a pull-down sets `out` to
                // the vLow rail when open. Ron«Rpd so the closed output sits at the rail.
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
                // Analog multiplier: out = gain·inA·inB. The portable building block of a control law
                // that scales one signal by another (here: a current reference shaped by |Vac| and scaled
                // by a voltage-loop gain). Every backend has a multiply block; ngspice = a B-source.
                const json& m = aas.at("multiplier");
                double gain = 1.0;
                if (m.contains("behavioral") && m.at("behavioral").is_object()
                    && m.at("behavioral").contains("gain")) gain = m.at("behavioral").at("gain").get<double>();
                const std::string inA = node_of(c.name, "inA");
                const std::string inB = node_of(c.name, "inB");
                const std::string out = node_of(c.name, "out");
                body << "B" << c.name << " " << out << " 0 V=" << num(gain) << "*V(" << inA << ")*V("
                     << inB << ")\n";
            }
            else if (aas.contains("summer")) {
                // Weighted summer / difference amp: out = gainA·V(inA) + gainB·V(inB). The portable
                // building block for a control error (e.g. a current reference minus a sensed current).
                // ngspice = a B-source.
                const json& s = aas.at("summer");
                auto opt = [&](const char* key, double dflt) -> double {
                    if (s.contains("behavioral") && s.at("behavioral").is_object()
                        && s.at("behavioral").contains(key)) return s.at("behavioral").at(key).get<double>();
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
                // Analog integrator / loop compensator: out = clamp(initial + gain·∫(in − reference)dt,
                // outputLow, outputHigh). The voltage-loop compensator (its `reference` is the setpoint,
                // baked in so no separate reference-source node is needed). Portable concept (op-amp
                // integrator); ngspice = a B-source with idt(), clamped against wind-up.
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
                // Integrate with the portable SPICE idiom: a behavioural CURRENT source = gain·(in−ref)
                // charging a 1 F capacitor, so V(raw) = initial + ∫gain·(in−ref)dt (the cap IC sets the
                // initial value under UIC). The output clamps V(raw) to [outputLow, outputHigh].
                body << "B" << c.name << "_i 0 " << raw << " I=" << num(gain) << "*(V(" << in << ")-("
                     << num(ref) << "))\n";
                body << "C" << c.name << "_int " << raw << " 0 1 IC=" << num(initial) << "\n";
                body << "B" << c.name << " " << out << " 0 V=(V(" << raw << ")<(" << num(lo) << "))?("
                     << num(lo) << "):((V(" << raw << ")>(" << num(hi) << "))?(" << num(hi) << "):V("
                     << raw << "))\n";
            }
            else {
                throw std::runtime_error("CIAS->ngspice: analog '" + c.name +
                                         "' block not yet supported (comparator / multiplier / integrator)");
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
