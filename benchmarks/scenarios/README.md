# Impairment scenarios

The benchmark matrix. Flat `key = value` files consumed by `tools/impair`;
see `impair --help` for the full key list.

Format note: the plan called for TOML. These are a flat key/value subset with
`#` comments, parsed by `tools/impair/scenario.cpp`, because the settings are
flat and a TOML dependency would buy nothing. Extension is `.conf` rather than
`.toml` so nobody expects tables or arrays to work.

Every run must record its **seed** alongside its results. Identical seed and
scenario replay a byte-identical impairment pattern, which is the entire
argument for this tool over `tc netem` — see `docs/decisions/`.
