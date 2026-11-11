# vstpreset-mcp

Python MCP server that wraps `vstparam_host` to inspect and control VST3 plugins
(native Linux bundles and **vstbridge**-bridged Wine VST3s) and to help generate presets
by researching online documentation and tutorials.

## Tools

| Tool | Description |
|---|---|
| `vst_list_parameters(path)` | List all parameters (parsed: index, id hex/decimal, title, default, value, units, flags) |
| `vst_find_parameters(path, query, limit=200)` | Filter parameters by title substring (case-insensitive) |
| `vst_set_parameter(path, param_id, value)` | Set parameter by ID (decimal or `0x` hex) to normalized `[0..1]` |
| `vst_save_snapshot(path, snapshot_path)` | Save preset snapshot in legacy text format |
| `vst_load_snapshot(path, snapshot_path)` | Load/apply snapshot and return resulting parameter listing |
| `web_search(query, max_results=8)` | DuckDuckGo HTML search for manuals/tutorials/preset recipes |
| `web_fetch(url, max_chars=20000)` | Fetch URL and extract readable text |

## Requirements
- Python 3.10+
- `mcp` (Model Context Protocol Python SDK, v2-compatible server implementation)
- `vstparam_host` binary available (default `/home/baboon/bin/vstparam_host`, override with `VSTPARAM_HOST`)

## Run (stdio)
```bash
VSTPARAM_HOST=/path/to/vstparam_host \
  python /path/to/vst3-preset/mcp-server/src/vstpreset_mcp/server.py
```

See the repository root `README.md` for full installation and client configuration (OpenCode, Claude Desktop, etc.).
