#!/usr/bin/env python3
"""
Example: Converting CIAS circuits to LTspice format.

This demonstrates how to use the PyCIAS converter to transform
CIAS circuit definitions into LTspice symbol and library files.
"""

import json
from pathlib import Path
import sys

# Add CIAS module to path
sys.path.insert(0, str(Path(__file__).parent.parent))

import PyCIAS


def example_rc_filter():
    """RC low-pass filter example."""
    cias = {
        "name": "rc_filter",
        "ports": [
            {"name": "in", "description": "Input signal"},
            {"name": "out", "description": "Filtered output"},
            {"name": "gnd", "description": "Ground reference"},
        ],
        "components": [
            {
                "name": "R1",
                "data": {
                    "type": "resistor",
                    "value": 1000.0,
                    "description": "Series resistor",
                },
            },
            {
                "name": "C1",
                "data": {
                    "type": "capacitor",
                    "value": 100e-9,
                    "description": "Shunt capacitor",
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
    return cias


def example_half_bridge():
    """Simple half-bridge topology."""
    cias = {
        "name": "half_bridge",
        "ports": [
            {"name": "Vin", "description": "DC input voltage"},
            {"name": "SW", "description": "Switching node (mid-point)"},
            {"name": "GND", "description": "Ground reference"},
        ],
        "components": [
            {
                "name": "Q1",
                "data": {
                    "type": "MOSFET",
                    "description": "High-side switch",
                },
            },
            {
                "name": "Q2",
                "data": {
                    "type": "MOSFET",
                    "description": "Low-side switch",
                },
            },
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
                "name": "mid_point",
                "endpoints": [
                    {"component": "Q1", "pin": "s"},
                    {"component": "Q2", "pin": "d"},
                    {"port": "SW"},
                ],
            },
            {
                "name": "gnd_rail",
                "endpoints": [
                    {"component": "Q2", "pin": "s"},
                    {"port": "GND"},
                ],
            },
        ],
    }
    return cias


def example_buck_input_filter():
    """Buck converter input LC filter."""
    cias = {
        "name": "buck_input_filter",
        "ports": [
            {"name": "Vin", "description": "Unfiltered input"},
            {"name": "Vbus", "description": "Filtered bus"},
            {"name": "GND", "description": "Ground"},
        ],
        "components": [
            {
                "name": "L1",
                "data": {
                    "type": "inductor",
                    "value": 10e-6,
                    "description": "Input series inductor",
                },
            },
            {
                "name": "C_in",
                "data": {
                    "type": "capacitor",
                    "value": 100e-6,
                    "description": "Input bulk capacitor",
                },
            },
            {
                "name": "C_bus",
                "data": {
                    "type": "capacitor",
                    "value": 47e-6,
                    "description": "Bus decoupling capacitor",
                },
            },
        ],
        "connections": [
            {
                "name": "vin",
                "endpoints": [
                    {"component": "L1", "pin": "1"},
                    {"port": "Vin"},
                ],
            },
            {
                "name": "lc_node",
                "endpoints": [
                    {"component": "L1", "pin": "2"},
                    {"component": "C_in", "pin": "1"},
                    {"component": "C_bus", "pin": "1"},
                    {"port": "Vbus"},
                ],
            },
            {
                "name": "gnd",
                "endpoints": [
                    {"component": "C_in", "pin": "2"},
                    {"component": "C_bus", "pin": "2"},
                    {"port": "GND"},
                ],
            },
        ],
    }
    return cias


def convert_and_display(cias, output_dir=None):
    """Convert a CIAS circuit and display the results."""
    print(f"\n{'='*60}")
    print(f"Converting: {cias['name']}")
    print(f"{'='*60}")

    asy_content, lib_content = PyCIAS.convert_cias_to_ltspice(cias)

    print("\n--- Generated .asy (Symbol File) ---")
    print(asy_content[:500] + ("..." if len(asy_content) > 500 else ""))

    print("\n--- Generated .lib (Subcircuit File) ---")
    print(lib_content[:500] + ("..." if len(lib_content) > 500 else ""))

    if output_dir:
        asy_path, lib_path = PyCIAS.save_cias_to_ltspice(cias, output_dir)
        print(f"\nSaved to:")
        print(f"  {asy_path}")
        print(f"  {lib_path}")

    return asy_content, lib_content


def main():
    """Run all examples."""
    # Create output directory
    output_dir = Path(__file__).parent / "output"
    output_dir.mkdir(exist_ok=True)

    # Example 1: RC Filter
    cias1 = example_rc_filter()
    convert_and_display(cias1, output_dir)

    # Example 2: Half Bridge
    cias2 = example_half_bridge()
    convert_and_display(cias2, output_dir)

    # Example 3: Buck Input Filter
    cias3 = example_buck_input_filter()
    convert_and_display(cias3, output_dir)

    print(f"\n{'='*60}")
    print("All examples completed!")
    print(f"Output files saved to: {output_dir}")
    print(f"{'='*60}\n")


if __name__ == "__main__":
    main()
