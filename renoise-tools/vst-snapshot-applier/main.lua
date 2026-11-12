--[[
  VST Snapshot Applier — Renoise Lua Tool (Automatic + Snapshot Loader)
  ====================================================================
  Reads a vstparam_host snapshot file and transfers parameter values to a VST
  plugin loaded in Renoise.

  Snapshot format (produced by `vstparam_host --save`):
    # VST3 parameter preset snapshot
    # format: <paramID decimal> <normalizedValue 0..1>
    1336600346 0.5
    110749 0.5

  Configuration (edit at top of file):
    SNAPSHOT_PATH   — default snapshot file (used if dialog is cancelled)
    VSTPARAM_HOST   — path to vstparam_host binary
    VST3_DIR        — directory to scan for .vst3 bundles (default: ~/.vst3)

  How it works:
    1. Opens a file picker to select a snapshot file
    2. Finds the first VST instrument in the Renoise song
    3. Auto-scans VST3_DIR for a .vst3 bundle matching the plugin name
    4. Runs vstparam_host to get param ID -> title mapping
    5. Matches titles to Renoise plugin_parameters by name
    6. Applies each normalized value (0..1)

  Usage:
    - Load a VST3 plugin (e.g. TAL-BassLine-101.vst3) as a Renoise instrument
    - Tools -> VST Snapshot Applier
    - Pick a snapshot file in the dialog
--]]

-- ================= CONFIGURE THESE =================
local SNAPSHOT_PATH  = "/tmp/tal-bassline-goa-classic.txt"   -- default snapshot file
local VSTPARAM_HOST  = "/home/baboon/bin/vstparam_host"      -- path to vstparam_host binary
local VST3_DIR       = os.getenv("HOME") .. "/.vst3"         -- directory to scan for .vst3 bundles
-- ===================================================

--------------------------------------------------------------------------------
-- Helpers
--------------------------------------------------------------------------------
local function shellescape(str)
  return "'" .. tostring(str):gsub("'", "'\\''") .. "'"
end

local function trim(s)
  return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

-- Normalize a name for comparison: strip non-alphanumeric, lowercase
-- e.g. "TAL BassLine 101" -> "talbassline101"
--      "TAL-BassLine-101" -> "talbassline101"
local function normalize_name(s)
  return tostring(s):lower():gsub("[^a-z0-9]", "")
end

--------------------------------------------------------------------------------
-- Parse snapshot file: returns { {id=NNN, value=0.5}, ... }
--------------------------------------------------------------------------------
local function parse_snapshot(path)
  local f = io.open(path, "r")
  if not f then
    return nil, "Cannot open snapshot: " .. tostring(path)
  end
  local entries = {}
  for line in f:lines() do
    line = trim(line)
    if line:sub(1, 1) ~= "#" and line:match("%S") then
      local id_str, val_str = line:match("(%d+)%s+([%d%.]+)")
      if id_str and val_str then
        table.insert(entries, {
          id = tonumber(id_str),
          value = tonumber(val_str),
        })
      end
    end
  end
  f:close()
  if #entries == 0 then
    return nil, "No parameter lines found in snapshot."
  end
  return entries
end

--------------------------------------------------------------------------------
-- Run vstparam_host, parse table output into id -> {index, title}
--------------------------------------------------------------------------------
local function get_vst_param_map(plugin_path)
  -- Check binary exists first
  local f = io.open(VSTPARAM_HOST, "r")
  if not f then
    return nil, "vstparam_host binary not found at: " .. VSTPARAM_HOST
  end
  f:close()

  local cmd = VSTPARAM_HOST .. " " .. shellescape(plugin_path)
  local handle = io.popen(cmd)
  if not handle then
    return nil, "Cannot run vstparam_host: " .. cmd
  end
  local output = handle:read("*a")
  handle:close()

  if not output or output == "" then
    return nil, "vstparam_host produced no output for: " .. plugin_path
  end

  local map = {}
  local count = 0
  for line in output:gmatch("[^\r\n]+") do
    -- Match: [  0] id=0x30 A:Lfo1_Rate def=0 value=0 units= flags=1
    local idx, id_val, title = line:match(
      "%[%s*(%d+)%]%s+id=(%S+)%s+(.-)%s+def="
    )
    if idx and id_val and title then
      local id_dec
      if id_val:sub(1, 2):lower() == "0x" then
        id_dec = tonumber(id_val, 16)
      else
        id_dec = tonumber(id_val)
      end
      if id_dec then
        map[id_dec] = {
          index = tonumber(idx),
          title = trim(title),
        }
        count = count + 1
      end
    end
  end

  if count == 0 then
    local preview = output:sub(1, 800)
    return nil, "vstparam_host returned no parseable parameters.\n\n"
      .. "Command: " .. cmd .. "\n"
      .. "Output preview:\n" .. preview
  end
  return map
end

--------------------------------------------------------------------------------
-- Auto-scan VST3_DIR for a bundle matching the plugin name
--------------------------------------------------------------------------------
local function find_vst3_bundle(plugin_name)
  local handle = io.popen("ls -1d " .. shellescape(VST3_DIR) .. "/*.vst3 2>/dev/null", "r")
  if not handle then
    return nil, "Cannot list VST3 directory: " .. VST3_DIR
  end
  local bundles = {}
  for line in handle:lines() do
    table.insert(bundles, trim(line))
  end
  handle:close()

  if #bundles == 0 then
    return nil, "No .vst3 bundles found in: " .. VST3_DIR
  end

  local name_norm = normalize_name(plugin_name)

  -- Try exact match first (normalized folder name == normalized plugin name)
  for _, bundle in ipairs(bundles) do
    local folder = bundle:match("([^/]+)%.vst3$")
    if folder and normalize_name(folder) == name_norm then
      return bundle, nil
    end
  end

  -- Try normalized contains match
  for _, bundle in ipairs(bundles) do
    local folder = bundle:match("([^/]+)%.vst3$")
    if folder then
      local folder_norm = normalize_name(folder)
      if name_norm:find(folder_norm, 1, true) or folder_norm:find(name_norm, 1, true) then
        return bundle, nil
      end
    end
  end

  -- No match found: list all available bundles
  local msg = "No .vst3 bundle matches '" .. tostring(plugin_name) .. "'.\n\n"
    .. "Available bundles in " .. VST3_DIR .. ":\n"
  for _, bundle in ipairs(bundles) do
    local folder = bundle:match("([^/]+)%.vst3$")
    msg = msg .. "  - " .. tostring(folder) .. "\n"
  end
  msg = msg .. "\nEdit VST3_DIR in main.lua, or ensure the plugin is installed there."
  return nil, msg
end

--------------------------------------------------------------------------------
-- Main
--------------------------------------------------------------------------------
local function apply_snapshot()
  local song = renoise.song()

  -- 1. Snapshot loader: pick a snapshot file (no extension appending)
  local snapshot_path = nil

  local vb = renoise.ViewBuilder()
  local button = renoise.app():show_custom_prompt(
    "VST Snapshot Applier — Select Snapshot",
    vb:column {
      vb:text { text = "Snapshot file:" },
      vb:textfield {
        id = "snapshot_path",
        text = SNAPSHOT_PATH,
      },
    },
    {"OK", "Cancel"}
  )
  if button == "OK" then
    local entered = trim(vb.views.snapshot_path.text)
    if entered and entered ~= "" then
      snapshot_path = entered
    end
  end

  if not snapshot_path then
    renoise.app():show_prompt(
      "VST Snapshot Applier",
      "No snapshot file selected.",
      {"OK"}
    )
    return
  end

  -- 2. Find the first VST instrument in the song
  local target_instr = nil
  for _, instr in ipairs(song.instruments) do
    if instr.plugin_properties then
      target_instr = instr
      break
    end
  end

  if not target_instr then
    renoise.app():show_prompt(
      "VST Snapshot Applier",
      "No VST instrument found in the song.\n\nLoad a VST3 plugin first.",
      {"OK"}
    )
    return
  end

  -- 3. Auto-detect plugin path by scanning ~/.vst3/
  local plugin_path, bundle_err = find_vst3_bundle(target_instr.name)
  if not plugin_path then
    renoise.app():show_prompt(
      "VST Snapshot Applier",
      bundle_err,
      {"OK"}
    )
    return
  end

  -- 4. Parse snapshot
  local entries, err = parse_snapshot(snapshot_path)
  if not entries then
    renoise.app():show_prompt("VST Snapshot Applier", err, {"OK"})
    return
  end

  -- 5. Get param ID -> title mapping from vstparam_host
  local param_map, err2 = get_vst_param_map(plugin_path)
  if not param_map then
    renoise.app():show_prompt("VST Snapshot Applier", err2, {"OK"})
    return
  end

  -- 6. Build Renoise name -> 1-based index map
  local plugin = target_instr.plugin
  local params = plugin and plugin.parameters
  if not params or #params == 0 then
    renoise.app():show_prompt(
      "VST Snapshot Applier",
      "Instrument '" .. tostring(target_instr.name)
        .. "' exposes no parameters.",
      {"OK"}
    )
    return
  end

  local name_to_idx = {}
  for i, p in ipairs(params) do
    name_to_idx[tostring(p.name):lower()] = i
  end

  -- 7. Apply values
  local applied = 0
  local skipped = 0
  local results = {}

  for _, entry in ipairs(entries) do
    local info = param_map[entry.id]
    if info then
      local rp_idx = name_to_idx[info.title:lower()]
      if rp_idx then
        local param = params[rp_idx]
        local v = math.min(1.0, math.max(0.0, entry.value))
        param.value = v
        applied = applied + 1
        table.insert(
          results,
          string.format("  %-32s = %.4f", info.title, v)
        )
      else
        skipped = skipped + 1
        table.insert(
          results,
          string.format(
            "  [SKIP] %-32s (title not in Renoise)",
            info.title
          )
        )
      end
    else
      skipped = skipped + 1
      table.insert(
        results,
        string.format("  [SKIP] ID %d (not in vstparam_host map)", entry.id)
      )
    end
  end

  local msg = string.format(
    "Applied %d / %d parameters to '%s'\nPlugin: %s\nSkipped: %d\n\n%s",
    applied,
    #entries,
    tostring(target_instr.name),
    plugin_path,
    skipped,
    table.concat(results, "\n"):sub(1, 3000)
  )

  renoise.app():show_prompt("VST Snapshot Applier — Done", msg, {"OK"})
end

renoise.tool():add_menu_entry {
  name = "Main Menu:Tools:VST Snapshot Applier",
  invoke = apply_snapshot
}
