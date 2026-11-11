# vst3-preset

A lightweight **VST3 host wrapper** (`vstparam_host`) plus an **MCP server** (`vstpreset-mcp`) for inspecting and controlling VST3 plugin parameters — including **vstbridge-bridged Wine VST3s** — and generating presets by researching online documentation and tutorials.

## Repository layout

- `vstparam-host/` — C++ VST3 host tool (Steinberg VST3 SDK hosting layer)
  - `source/vstparam_host.cpp` — host implementation
  - `build.sh` — build script (compiles against VST3 SDK sources)
- `mcp-server/` — Python MCP server wrapping `vstparam_host` + web research tools
  - `src/vstpreset_mcp/server.py` — MCP server (stdio)
  - `pyproject.toml`

## Features

### `vstparam_host`
- Takes any VST3 bundle path and **lists all parameters** (id, title, default, current value, units, flags)
- **Set** a parameter by ID to a normalized value `[0..1]`
- **Save/load** preset snapshots in a simple, pipeline-friendly text format
- Works with:
  - native Linux VST3 bundles
  - **vstbridge**-wrapped Windows VST3 plugins (the SDK loader dlopens the wrapper `.so`, which hands off to vstbridge/Wine)

### `vstpreset-mcp` (MCP server)
Exposes tools so an MCP-capable agent can:
- enumerate/find/set/save/load VST3 parameters
- search the web for manuals/tutorials/preset recipes
- fetch and read pages to map documented settings onto actual plugin parameter IDs/values

## Prerequisites

- Linux, `g++` (C++17-capable), `pthread`, `dl`
- [Steinberg VST3 SDK](https://github.com/steinbergmedia/vst3sdk) source checkout (for building the host)
- Python 3.10+ and the Model Context Protocol Python SDK (`mcp`) for the MCP server
- (Optional, for bridged Windows VST3s) [vstbridge](https://github.com/robbert-vdh/yabridge#vstbridge) / Wine with bridged `.vst3` wrappers installed

## Installation

### 1. Build `vstparam_host`

```bash
git clone --recursive https://github.com/steinbergmedia/vst3sdk.git
# or point VST3_SDK at an existing checkout

cd vst3-preset/vstparam-host
chmod +x build.sh
VST3_SDK=/path/to/vst3sdk ./build.sh            # outputs ./vstparam_host
# or: ./build.sh /path/to/vst3sdk /usr/local/bin/vstparam_host
```

Install/copy the binary somewhere on your PATH or note its absolute path (the MCP server uses it):

```bash
install -m 0755 ./vstparam_host ~/.local/bin/vstparam_host
```

### 2. Install the MCP server

Using a virtual environment (recommended):

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install "mcp[cli]>=1.2.0"
# (optional, if you want an editable install of the package metadata)
# pip install -e ./mcp-server
```

Run directly without packaging:

```bash
VSTPARAM_HOST=/absolute/path/to/vstparam_host \
  .venv/bin/python /absolute/path/to/vst3-preset/mcp-server/src/vstpreset_mcp/server.py
```

> **Note:** The server is implemented for the MCP Python SDK v2 server API (`mcp.server.mcpserver.MCPServer`). Use a recent `mcp` package (v2). The `VSTPARAM_HOST` environment variable overrides the host binary path (default: `/home/baboon/bin/vstparam_host`).

## `vstparam_host` usage

```bash
# List all parameters
vstparam_host /path/to/Plugin.vst3

# Set a parameter (ID accepts decimal or 0x hex), normalized value [0..1]
vstparam_host /path/to/Plugin.vst3 --set 0x4faae71a 0.25

# Save a preset snapshot
vstparam_host /path/to/Plugin.vst3 --save /tmp/preset.txt

# Load a preset snapshot (applies values, then lists parameters)
vstparam_host /path/to/Plugin.vst3 --load /tmp/preset.txt
```

**Output (list):**
```text
plug-in: TAL-EQ | vendor: TAL-Togu Audio Line | version: 1.3.2
[  0] id=0x2e186ce8 On / Off def=0 value=0 units= flags=1
[  1] id=0x4faae71a Volume def=0.5 value=0.5 units= flags=1
...
```

**Snapshot format** (legacy-compatible, one parameter per line):
```text
# VST3 parameter preset snapshot
# format: <paramID decimal> <normalizedValue 0..1>
773352680 0
1336600346 0.5
...
```

> **SDK quirk:** pass bundle paths **without a trailing slash** (the SDK derives the inner `.so` path from the bundle stem).

## MCP server setup

The MCP server communicates over **stdio** and must be launched by your MCP-capable client.

### Tools provided

| Tool | Description |
|---|---|
| `vst_list_parameters(path)` | List all parameters with parsed metadata (index, id hex/decimal, title, default, value, units, flags) |
| `vst_find_parameters(path, query, limit=200)` | Find parameters by title substring (case-insensitive) |
| `vst_set_parameter(path, param_id, value)` | Set parameter by ID (decimal or `0x` hex) to normalized `[0..1]` |
| `vst_save_snapshot(path, snapshot_path)` | Save all parameters to a snapshot file |
| `vst_load_snapshot(path, snapshot_path)` | Load/apply a snapshot and list resulting values |
| `web_search(query, max_results=8)` | Search web for manuals/tutorials/preset recipes (DuckDuckGo HTML) |
| `web_fetch(url, max_chars=20000)` | Fetch a page and extract readable text |

### OpenCode (`opencode.json`)

Add under the top-level `"mcp"` object (config is not hot-reloaded — restart OpenCode after changes):

```json
{
  "mcp": {
    "vstpreset": {
      "type": "local",
      "command": [
        "/absolute/path/to/.venv/bin/python",
        "/absolute/path/to/vst3-preset/mcp-server/src/vstpreset_mcp/server.py"
      ],
      "environment": {
        "VSTPARAM_HOST": "/absolute/path/to/vstparam_host"
      }
    }
  }
}
```

### Claude Desktop (`claude_desktop_config.json`)

```json
{
  "mcpServers": {
    "vstpreset": {
      "command": "/absolute/path/to/.venv/bin/python",
      "args": [
        "/absolute/path/to/vst3-preset/mcp-server/src/vstpreset_mcp/server.py"
      ],
      "env": {
        "VSTPARAM_HOST": "/absolute/path/to/vstparam_host"
      }
    }
  }
}
```

### Generic MCP clients

Launch as a stdio server:

```bash
VSTPARAM_HOST=/absolute/path/to/vstparam_host \
  /absolute/path/to/.venv/bin/python \
  /absolute/path/to/vst3-preset/mcp-server/src/vstpreset_mcp/server.py
```

## Preset-generation workflow (research → map → apply)

1. **Inspect the plugin**
   - `vst_list_parameters(path)` to enumerate all parameters and their IDs/defaults.
   - `vst_find_parameters(path, "cutoff")` (etc.) to locate relevant controls.
2. **Research**
   - `web_search("<plugin name> <target sound> preset settings tutorial/manual")`.
   - `web_fetch(url)` on promising results to extract recommended settings, ranges, and intent.
3. **Map & apply**
   - Translate documented values into normalized `[0..1]` for the matching parameter IDs.
   - Apply with `vst_set_parameter` (or prepare a snapshot file with `<paramID decimal> <normalizedValue>` lines).
4. **Persist & verify**
   - `vst_save_snapshot(path, snapshot_path)` to save a reusable preset.
   - `vst_load_snapshot(path, snapshot_path)` to reapply and confirm resulting values.

**Example prompts for an agent:**
- "List parameters for `/home/me/.vst3/TAL-EQ.vst3`, search for a warm vocal EQ recipe, map it to parameter IDs, set them, and save `/tmp/tal-warm-vocal.preset.txt`."
- "Generate a preset for the bridged `/home/me/.vst3/vstbridge/Jup-8 V4.vst3` by checking online tutorials, then save a snapshot."

## vstbridge (bridged Wine VST3) support

Bridged Windows VST3s exposed via vstbridge work through the same host path: the SDK module loader dlopens the bridge wrapper's `Contents/<arch>/<name>.so`, whose `GetPluginFactory` forwards to the vstbridge/Wine layer. Point `vstparam_host`/MCP tools at the bridged `.vst3` bundle (e.g. `~/.vst3/vstbridge/Plugin.vst3`) just like a native bundle.

## Notes & limitations

- Parameter values are handled as **normalized** `[0..1]` (VST3 controller semantics).
- Snapshots capture/apply controller parameter values in the legacy text format for compatibility; they are not `.vstpreset` container files.
- Only the VST3 **Audio Module Class** is enumerated.
- `web_search`/`web_fetch` are best-effort (DuckDuckGo HTML/network variability); combine with other research sources if needed.
- `vstparam_host` is Linux-oriented in this build script (`module_linux.cpp`, `linuxmain.cpp`); adapting to other platforms requires the corresponding SDK platform sources.

## License

`vstparam_host` is built against the Steinberg VST3 SDK — ensure your use/build complies with the [VST3 SDK licensing terms](https://www.steinberg.net/developers/). This repository does not include the VST3 SDK. Add a project license file as appropriate for your distribution.
