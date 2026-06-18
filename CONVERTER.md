# CIAS ↔ LTspice Converter

A C++ library with Python bindings for bidirectional conversion between CIAS (Circuit Agnostic Structure) and LTspice formats.

## Overview

The converter translates between:
- **CIAS JSON**: Vendor/simulator-neutral circuit brick definition
- **LTspice Files**: 
  - `.asy` — Symbol definition (schematic interface)
  - `.lib` — Subcircuit definition (SPICE behavioral model)

## Architecture

### C++ Core (`src/CiasConverter.hpp/cpp`)

**Data Structures:**
- `Port` — External terminal of the brick
- `Component` — Placed part with reference designator and PEAS data
- `Endpoint` — Either a pin on a component or an external port
- `Connection` — Electrical net with 2+ endpoints
- `CiasCircuit` — Complete brick definition

**Converters:**

1. **CiasToLtspiceConverter**
   - `to_asy()` — Generate LTspice symbol (.asy)
   - `to_lib()` — Generate SPICE subcircuit (.lib)
   - `convert()` — Both files as a pair
   - `convert_json()` — From JSON input

2. **LtspiceToRawConverter**
   - `from_ltspice_files()` — Parse .asy and .lib to `CiasCircuit`
   - `from_ltspice_files_json()` — Same, returns CIAS JSON
   - `asy_to_json()` — Extract pins from symbol
   - `lib_to_json()` — Extract ports and components from subcircuit

### Python Bindings (`src/bindings.cpp`)

Built with pybind11, exposing:
```python
from PyCIAS import (
    CiasCircuit,
    CiasToLtspiceConverter,
    LtspiceToRawConverter,
    convert_cias_to_ltspice,
    convert_ltspice_to_cias,
)
```

## Installation

### From Source

```bash
cd CIAS
pip install -e .
```

### Development Build

```bash
pip install scikit-build-core pybind11 cmake
cd CIAS
pip install -e . --no-build-isolation -v
```

## Usage

### Python API

#### Basic Conversion

```python
from PyCIAS import convert_cias_to_ltspice, convert_ltspice_to_cias

# CIAS → LTspice
cias_circuit = {
    "name": "rc_filter",
    "ports": [
        {"name": "in"},
        {"name": "out"},
        {"name": "gnd"},
    ],
    "components": [
        {"name": "R1", "data": {"type": "resistor", "value": 1000.0}},
        {"name": "C1", "data": {"type": "capacitor", "value": 1e-6}},
    ],
    "connections": [
        {
            "name": "net_in",
            "endpoints": [
                {"component": "R1", "pin": "1"},
                {"port": "in"},
            ],
        },
        {
            "name": "net_out",
            "endpoints": [
                {"component": "R1", "pin": "2"},
                {"component": "C1", "pin": "1"},
                {"port": "out"},
            ],
        },
        {
            "name": "net_gnd",
            "endpoints": [
                {"component": "C1", "pin": "2"},
                {"port": "gnd"},
            ],
        },
    ],
}

asy_content, lib_content = convert_cias_to_ltspice(cias_circuit)
print(asy_content)  # LTspice symbol file
print(lib_content)  # SPICE subcircuit definition
```

#### File I/O Helpers

```python
import PyCIAS

# Save CIAS to LTspice files
asy_path, lib_path = PyCIAS.save_cias_to_ltspice(cias_circuit, output_dir="./ltspice_models")
print(f"Saved: {asy_path}, {lib_path}")

# Load LTspice files to CIAS
cias_from_files = PyCIAS.load_ltspice_to_cias("circuit.asy", "circuit.lib")
print(cias_from_files)
```

#### Class-Based API

```python
from PyCIAS import CiasCircuit, CiasToLtspiceConverter

# Create a circuit from JSON
circuit = CiasCircuit.from_json(cias_circuit)

# Access properties
print(f"Circuit name: {circuit.name}")
print(f"Ports: {[p.name for p in circuit.ports]}")
print(f"Components: {[c.name for c in circuit.components]}")

# Convert using class
converter = CiasToLtspiceConverter()
asy, lib = converter.convert(circuit)
```

### CIAS JSON Format

**Minimal Example:**

```json
{
  "name": "my_brick",
  "ports": [
    {"name": "Vin"},
    {"name": "Vout"},
    {"name": "GND"}
  ],
  "components": [
    {"name": "R1", "data": {"type": "resistor", "value": 10000}},
    {"name": "C1", "data": {"type": "capacitor", "value": "100u"}}
  ],
  "connections": [
    {
      "name": "vdd",
      "endpoints": [
        {"component": "R1", "pin": "1"},
        {"port": "Vin"}
      ]
    },
    {
      "name": "vout_net",
      "endpoints": [
        {"component": "R1", "pin": "2"},
        {"component": "C1", "pin": "1"},
        {"port": "Vout"}
      ]
    },
    {
      "name": "gnd_net",
      "endpoints": [
        {"component": "C1", "pin": "2"},
        {"port": "GND"}
      ]
    }
  ]
}
```

**Ports Array:**
- Array of terminal definitions
- Each port has `name` (required) and optional `description`
- External connection studs of the brick

**Components Array:**
- Array of placed parts
- Each component has:
  - `name` — Reference designator (local to brick, e.g., "Q1", "C_in")
  - `data` — Either inline PEAS object or URI string (e.g., "TAS/data/mosfets.ndjson?partNumber=...")

**Connections Array:**
- Array of electrical nets
- Each connection has:
  - `name` — Net name (local to brick)
  - `endpoints` — Array of 2+ endpoints, each either:
    - **pinEndpoint**: `{"component": "...", "pin": "..."}` — specific component pin
    - **portEndpoint**: `{"port": "..."}` — external port (exposes net there)

## LTspice Output Format

### .asy Symbol File

Standard LTspice symbol format:
```
Version 4
SymbolType BLOCK
RECTANGLE Normal 64 48 304 240
PIN 0 64 LEFT 36
PINATTR PinName in
PINATTR SpiceOrder 1
PIN 0 96 LEFT 36
PINATTR PinName out
PINATTR SpiceOrder 2
PIN 0 128 LEFT 36
PINATTR PinName gnd
PINATTR SpiceOrder 3
TEXT 32 144 LEFT 3 rc_filter
```

### .lib Subcircuit File

Standard SPICE subcircuit:
```spice
.subckt rc_filter in out gnd
* Component: R1
Rrc_filter_R1 in net_out 1k
* Component: C1
Crc_filter_C1 net_out gnd 1u
.ends rc_filter
```

## Component Type Support

Currently supported in conversion:

| Type | Examples | Output |
|------|----------|--------|
| Resistor | `"resistor"`, `"Resistor"`, `"R"` | `R` prefix |
| Capacitor | `"capacitor"`, `"Capacitor"`, `"C"` | `C` prefix |
| Inductor | `"inductor"`, `"Inductor"`, `"L"` | `L` prefix |
| Other | Any other type | `X` prefix (subcircuit) |

Values can be:
- Numbers: `1000`, `1e-6`
- Strings: `"10k"`, `"100u"`, `"1.5M"`

## Design Notes

### CIAS → LTspice

1. **Port Ordering**: Ports are exported in declaration order
2. **Node Names**: Uses connection names as net identifiers
3. **Pin Numbering**: Extracted from component endpoint pins
4. **Symbol Geometry**: Default 64×176 pixel layout with evenly-spaced pins

### LTspice → CIAS

1. **Pin Extraction**: Parses PINATTR lines from .asy files
2. **Port Detection**: Extracts from `.subckt` declaration line
3. **Component Parsing**: Identifies R/C/L/X declarations
4. **Limitations**: 
   - Does not reconstruct complete connections (only component names and ports)
   - Requires manual refinement for complex topologies

## Testing

Run the test suite:

```bash
cd CIAS
pytest tests/test_conversion.py -v
```

## Building for Distribution

Create a wheel:

```bash
cd CIAS
pip install build
python -m build
```

Output wheel in `dist/PyCIAS-*.whl`

## Integration with Heimdall

This converter is intended as a subcomponent of the Heimdall project's CIAS handling. It bridges between:

1. **Heimdall Data**: CIAS circuit definitions stored as JSON
2. **LTspice Library**: Würth Elektronik LTspice symbols and models
3. **Simulation Tools**: Bridge to SPICE simulators

## TODO / Roadmap

- [ ] Support for transformer coupling definitions
- [ ] Advanced symbol layout options (pin positioning, labels)
- [ ] Reverse-conversion polish (inference of component types from .lib)
- [ ] Integration with PEAS component resolver
- [ ] Test harness for round-trip conversion fidelity
