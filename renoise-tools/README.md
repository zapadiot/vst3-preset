# Renoise Tools

Renoise Lua tools for working with `vstparam_host` snapshots.

## VST Snapshot Applier

Reads a `vstparam_host` snapshot file (`# format: <paramID decimal> <normalizedValue 0..1>`)
and transfers the values into a VST plugin loaded in Renoise.

### Folder
```
renoise-tools/vst-snapshot-applier/
├── manifest.xml
└── main.lua
```

### Install
Copy the folder `vst-snapshot-applier` into your Renoise tools directory:

```bash
# Linux
cp -r renoise-tools/vst-snapshot-applier ~/.renoise/Tools/
```

Then restart Renoise. Find it under **Tools → VST Snapshot Applier**.

### Configure
Edit `main.lua` at the top:
```lua
local SNAPSHOT_PATH  = "/tmp/tal-pha-lush-pad.txt"
local VSTPARAM_HOST  = "/home/baboon/bin/vstparam_host"
```

### Workflow
1. Load a VST3 plugin (e.g. TAL-Pha.vst3) as a Renoise instrument
2. Generate a snapshot with `vstparam_host`:
   ```bash
   vstparam_host /path/to/Plugin.vst3 --save /tmp/my-preset.txt
   ```
3. (Optional) Edit `/tmp/my-preset.txt` values
4. Run **Tools → VST Snapshot Applier** in Renoise
5. Values are applied to matching parameters by title

### Notes
- Matches parameters by title name (not by index), so ordering differences are fine.
- `vstparam_host` controls parameters only; it does not render audio. The VST
  must be loaded in Renoise (or another DAW) to hear it.
- Only the first VST instrument in the song is targeted. Edit the script to
  target a specific instrument index if needed.
