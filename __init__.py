"""CIAS (Circuit Agnostic Structure) Python bindings."""

import json as json_module
from pathlib import Path

try:
    from .PyCIAS import (
        Port,
        Endpoint,
        Component,
        Connection,
        CiasCircuit,
        CiasToLtspiceConverter,
        LtspiceToRawConverter,
        convert_cias_to_ltspice,
        convert_ltspice_to_cias,
    )
except ImportError:
    raise ImportError(
        "Failed to import PyCIAS bindings. "
        "Build with: pip install -e . "
        "or: python -m pip install --upgrade --force-reinstall ."
    )


def save_cias_to_ltspice(cias_dict, output_dir):
    """
    Convert a CIAS dictionary to LTspice files and save them.

    Args:
        cias_dict: CIAS circuit as a dictionary
        output_dir: Directory to save .asy and .lib files

    Returns:
        Tuple of (asy_path, lib_path)
    """
    import json

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    asy_content, lib_content = convert_cias_to_ltspice(cias_dict)
    circuit_name = cias_dict.get("name", "circuit")

    asy_path = output_dir / f"{circuit_name}.asy"
    lib_path = output_dir / f"{circuit_name}.lib"

    with open(asy_path, "w") as f:
        f.write(asy_content)

    with open(lib_path, "w") as f:
        f.write(lib_content)

    return str(asy_path), str(lib_path)


def load_ltspice_to_cias(asy_path, lib_path):
    """
    Convert LTspice files to CIAS format.

    Args:
        asy_path: Path to .asy symbol file
        lib_path: Path to .lib library file

    Returns:
        CIAS circuit as a dictionary
    """
    with open(asy_path) as f:
        asy_content = f.read()

    with open(lib_path) as f:
        lib_content = f.read()

    return convert_ltspice_to_cias(asy_content, lib_content)


__all__ = [
    "Port",
    "Endpoint",
    "Component",
    "Connection",
    "CiasCircuit",
    "CiasToLtspiceConverter",
    "LtspiceToRawConverter",
    "convert_cias_to_ltspice",
    "convert_ltspice_to_cias",
    "save_cias_to_ltspice",
    "load_ltspice_to_cias",
]
