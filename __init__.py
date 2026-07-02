"""CIAS (Circuit Agnostic Structure) Python bindings.

The current API is the multi-simulator PEAS-atom converter:

    from PyCIAS import CiasCircuitConverter, CircuitSimulator, convert_cias_to_ngspice
    netlist = convert_cias_to_ngspice(cias_dict)

The legacy LTspice .asy/.lib exporter and the LTspice->CIAS extractor were removed:
their data shapes (`ltspice_declaration`, `spice_params`) are illegal against
CIAS.json and live on only as an internal format of the Heimdall extraction
pipeline (see the ABT brief in eb-modelling-heimdall).
"""

try:
    from .PyCIAS import (
        Port,
        Endpoint,
        Component,
        Connection,
        CiasCircuit,
        CircuitSimulator,
        CiasCircuitConverter,
        CiasToNgspiceConverter,
        convert_cias_to_simulator,
        convert_cias_to_ngspice,
        convert_cias_to_ltspice_subckt,
        validate_cias_structure,
        validate_cias_structure_json,
    )
except ImportError:
    raise ImportError(
        "Failed to import PyCIAS bindings. "
        "Build with: pip install -e . "
        "or: python -m pip install --upgrade --force-reinstall ."
    )

__all__ = [
    "Port",
    "Endpoint",
    "Component",
    "Connection",
    "CiasCircuit",
    "CircuitSimulator",
    "CiasCircuitConverter",
    "CiasToNgspiceConverter",
    "convert_cias_to_simulator",
    "convert_cias_to_ngspice",
    "convert_cias_to_ltspice_subckt",
    "validate_cias_structure",
    "validate_cias_structure_json",
]
