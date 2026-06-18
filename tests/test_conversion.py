"""Tests for CIAS conversion functionality."""

import json
import pytest
from pathlib import Path


def test_cias_to_ltspice_basic():
    """Test basic CIAS to LTspice conversion."""
    from PyCIAS import convert_cias_to_ltspice

    cias_circuit = {
        "name": "rc_filter",
        "ports": [
            {"name": "in", "description": "Input"},
            {"name": "out", "description": "Output"},
            {"name": "gnd", "description": "Ground"},
        ],
        "components": [
            {
                "name": "R1",
                "data": {
                    "type": "resistor",
                    "value": 1000.0,
                },
            },
            {
                "name": "C1",
                "data": {
                    "type": "capacitor",
                    "value": 1e-6,
                },
            },
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

    assert "rc_filter" in asy_content
    assert "PINATTR PinName in" in asy_content or "in" in asy_content
    assert ".subckt rc_filter" in lib_content
    assert "R1" in lib_content
    assert "C1" in lib_content


def test_ciastocircuit_creation():
    """Test CiasCircuit object creation from JSON."""
    from PyCIAS import CiasCircuit

    cias_json = {
        "name": "simple_circuit",
        "ports": [{"name": "p1"}, {"name": "p2"}],
        "components": [{"name": "R1", "data": {"type": "resistor", "value": 100}}],
        "connections": [
            {
                "name": "net1",
                "endpoints": [
                    {"component": "R1", "pin": "1"},
                    {"port": "p1"},
                ],
            }
        ],
    }

    circuit = CiasCircuit.from_json(cias_json)
    assert circuit.name == "simple_circuit"
    assert len(circuit.ports) == 2
    assert len(circuit.components) == 1
    assert len(circuit.connections) == 1

    roundtrip_json = circuit.to_json()
    assert roundtrip_json["name"] == "simple_circuit"


def test_converter_instance():
    """Test creating converter instances."""
    from PyCIAS import CiasToLtspiceConverter, LtspiceToRawConverter

    converter = CiasToLtspiceConverter()
    assert converter is not None

    ltspice_converter = LtspiceToRawConverter()
    assert ltspice_converter is not None


def test_half_bridge_example():
    """Test a more complex circuit (half-bridge)."""
    from PyCIAS import convert_cias_to_ltspice

    half_bridge = {
        "name": "half_bridge",
        "ports": [
            {"name": "Vin"},
            {"name": "SW"},
            {"name": "GND"},
        ],
        "components": [
            {"name": "Q1", "data": {"type": "MOSFET"}},
            {"name": "Q2", "data": {"type": "MOSFET"}},
        ],
        "connections": [
            {
                "name": "vdd",
                "endpoints": [
                    {"component": "Q1", "pin": "d"},
                    {"port": "Vin"},
                ],
            },
            {
                "name": "sw_node",
                "endpoints": [
                    {"component": "Q1", "pin": "s"},
                    {"component": "Q2", "pin": "d"},
                    {"port": "SW"},
                ],
            },
            {
                "name": "gnd",
                "endpoints": [
                    {"component": "Q2", "pin": "s"},
                    {"port": "GND"},
                ],
            },
        ],
    }

    asy_content, lib_content = convert_cias_to_ltspice(half_bridge)

    assert "half_bridge" in lib_content
    assert ".subckt half_bridge" in lib_content
    assert "Vin" in lib_content
    assert "SW" in lib_content
    assert "GND" in lib_content


if __name__ == "__main__":
    test_cias_to_ltspice_basic()
    test_ciastocircuit_creation()
    test_converter_instance()
    test_half_bridge_example()
    print("All tests passed!")
