# CIAS — Circuit Agnostic Structure

> A vendor- and simulator-neutral description of a **reusable circuit brick** — the Lego piece that converter designs (TAS) snap together.

CIAS is a [JSON Schema 2020-12](https://json-schema.org/draft/2020-12/schema) data model for one **circuit brick**: a list of components and how they connect, plus the external terminals (ports) through which the brick wires to other bricks. It carries *only* circuit structure — no role, no "why", no stimulus, no analysis.

The SPICE analogy is exact: **a CIAS document is a `.subckt`** — a definition with a port list and internal elements, **not runnable by itself**. A [TAS](https://github.com/Power-Supply-Manufacturers-Association/TAS) document is the runnable deck: it instantiates CIAS bricks as stages, types and wires their terminals, and adds stimulus + analyses.

## Where it sits

```
TAS    the minimal simulatable converter deck     (stages + wiring + stimulus + analyses)
 │  stage.circuit  →  inline CIAS  |  "TAS/data/circuits.ndjson?name=half-bridge"
 ▼
CIAS   a reusable circuit brick                    (ports + components + connections)
 │  component.data →  inline PEAS  |  "TAS/data/mosfets.ndjson?partNumber=…"
 ▼
PEAS   a single part                               (magnetic | semiconductor | capacitor | resistor)
```

Each tier references the tier below **inline or by URI** — the same indirection everywhere. CIAS is a peer of TAS above the [PEAS](https://github.com/Power-Supply-Manufacturers-Association/PEAS) component families.

## The brick

`schemas/CIAS.json` — `{ name, ports[], components[], connections[] }`, all required, `additionalProperties: false`. Names (port / component / connection) are **local to the brick**.

| Field | Shape |
|-------|-------|
| `name` | brick type name (the lookup key for `?name=…` references) |
| `ports[]` | `{name, description?}` — external terminals (the studs) |
| `components[]` | `{name, data: oneOf[ PEAS doc \| URI ]}` |
| `connections[]` | `{name, endpoints[≥2]}` — one electrical net |

`endpoint` = `oneOf[ pinEndpoint{component,pin} \| portEndpoint{port} ]`. A net that includes a `portEndpoint` is **exposed** at that brick terminal.

There are **no magnetic-coupling edges**: a coupled inductor or transformer is a single multi-winding PEAS component, whose coupling lives inside its own (MAS) model.

## Cross-repo `$ref` resolution

Schemas reference each other by absolute `$id` URI (`https://psma.com/<repo>/<file>.json`), not by relative path. To validate CIAS documents you need the sibling repos (at least PEAS) checked out alongside this one. Reusable bricks live in the TAS repo's `data/circuits.ndjson`.

## Validation

```bash
pip install pytest jsonschema referencing
```

CIAS bricks are validated structurally against `schemas/CIAS.json`, and for referential integrity (every `portEndpoint`/`pinEndpoint` resolves, no port left unwired, names unique) by `TAS/scripts/validate_topology.py` in the TAS repo. See `TAS/docs/schema.md` for the full TAS↔CIAS model.

## License

Apache-2.0 — see [LICENSE.md](LICENSE.md).
