--[[
  VST Snapshot Applier — Renoise Lua Tool (Automatic)
  ====================================================
  Reads a vstparam_host snapshot file and transfers parameter values to a VST
  plugin loaded in Renoise — fully automatic, no dialogs.

  Snapshot format (produced by `vstparam_host --save`):
    # VST3 parameter preset snapshot
    # format: <paramID decimal> <normalizedValue 0..1>
    1336600346 0.5
    110749 0.5

  Configuration (edit at top of file):
    SNAPSHOT_PATH   — snapshot file to read (default: /tmp/tal-bassline-goa-classic.txt)
    VSTPARAM_HOST   — path to vstparam_host binary
    VST3_DIR        — directory to scan for .vst3 bundles (default: ~/.vst3)

  How it works:
    1. Finds the first VST instrument in the Renoise song
    2. Scans VST3_DIR for a bundle matching the plugin name
    3. Parses the snapshot file
    4. Runs vstparam_host to get param ID -> title mapping
    5. Matches titles to Renoise plugin_parameters by name
    6. Applies each normalized value (0..1)

  Usage:
    - Load a VST3 plugin (e.g. TAL-BassLine-101.vst3) as a Renoise instrument
    - Tools -> VST Snapshot Applier (runs automatically, no prompts)
--]]

-- ================= CONFIGURE THESE =================
local SNAPSHOT_PATH  = "/tmp/tal-bassline-goa-classic.txt"   -- snapshot file to read
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
  local cmd = VSTPARAM_HOST .. " " .. shellescape(plugin_path)
  local handle = io.popen(cmd)
  if not handle then
    return nil, "Cannot run vstparam_host (" .. cmd .. ")"
  end
  local output = handle:read("*a")
  handle:close()

  local map = {}
  for line in output:gmatch("[^\r\n]+") do
    -- matches:  [  0] id=0x2e186ce8 On / Off def=0 value=0 units= flags=1
    local idx, id_hex, title = line:match(
      "%[%s*(%d+)%]%s+id=(0[xX][0-9a-fA-F]+|%d+)%s+(.-)%s+def="
    )
    if idx and id_hex and title then
      local id_dec
      if id_hex:sub(1, 2):lower() == "0x" then
        id_dec = tonumber(id_hex, 16)
      else
        id_dec = tonumber(id_hex)
      end
      if id_dec then
        map[id_dec] = {
          index = tonumber(idx),
          title = trim(title),
        }
      end
    end
  end

  if next(map) == nil then
    return nil, "vstparam_host returned no parseable parameters."
  end
  return map
end

--------------------------------------------------------------------------------
-- Auto-scan VST3_DIR for a bundle matching the plugin name
--------------------------------------------------------------------------------
local function find_vst3_bundle(plugin_name)
  local handle = io.popen("ls -1d " .. shellescape(VST3_DIR) .. "/*.vst3 2>/dev/null", "r")
  if not handle then
    return nil
  end
  local bundles = {}
  for line in handle:lines() do
    table.insert(bundles, trim(line))
  end
  handle:close()

  if #bundles == 0 then
    return nil
  end

  local name_lower = tostring(plugin_name):lower()

  -- Try exact match first (folder name == plugin name)
  for _, bundle in ipairs(bundles) do
    local folder = bundle:match("([^/]+)%.vst3$")
    if folder and folder:lower() == name_lower then
      return bundle
    end
  end

  -- Try: plugin name contains folder name or vice versa
  for _, bundle in ipairs(bundles) do
    local folder = bundle:match("([^/]+)%.vst3$")
    if folder then
      local folder_lower = folder:lower()
      if name_lower:find(folder_lower, 1, true) or folder_lower:find(name_lower, 1, true) then
        return bundle
      end
    end
  end

  -- Fallback: return first bundle
  return bundles[1]
end

--------------------------------------------------------------------------------
-- Main
--------------------------------------------------------------------------------
local function apply_snapshot()
  local song = renoise.song()

  -- 1. Find the first VST instrument in the song
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

  -- 2. Auto-detect plugin path by scanning ~/.vst3/
  local plugin_path = find_vst3_bundle(target_instr.name)
  if not plugin_path then
    renoise.app():show_prompt(
      "VST Snapshot Applier",
      "Could not auto-find .vst3 bundle for '" .. tostring(target_instr.name) .. "'.\n\n"
        .. "Searched: " .. VST3_DIR .. "\n\n"
        .. "Ensure the plugin is installed there.",
      {"OK"}
    )
    return
  end

  -- 3. Parse snapshot
  local entries, err = parse_snapshot(SNAPSHOT_PATH)
  if not entries then
    renoise.app():show_prompt("VST Snapshot Applier", err, {"OK"})
    return
  end

  -- 4. Get param ID -> title mapping from vstparam_host
  local param_map, err2 = get_vst_param_map(plugin_path)
  if not param_map then
    renoise.app():show_prompt("VST Snapshot Applier", err2, {"OK"})
    return
  end

  -- 5. Build Renoise name -> 1-based index map
  local params = target_instr.plugin_parameters
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

  -- 6. Apply values
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
