--[[
  VST Snapshot Applier — Renoise Lua Tool
  =======================================
  Reads a vstparam_host snapshot file and transfers parameter values to a VST
  plugin loaded in Renoise.

  Snapshot format (produced by `vstparam_host --save`):
    # VST3 parameter preset snapshot
    # format: <paramID decimal> <normalizedValue 0..1>
    1336600346 0.5
    110749 0.5

  How it works:
    1. Finds the first VST instrument in the Renoise song
    2. Reads its .vst3 file path from Renoise
    3. Parses the snapshot file (param ID + normalized value)
    4. Runs `vstparam_host <path>` to get param ID -&gt; parameter title mapping
    5. Matches titles to Renoise plugin_parameters by name
    6. Applies each normalized value (0..1)

  Usage:
    - Load a VST3 plugin (e.g. TAL-Pha.vst3) as a Renoise instrument
    - Open Options -&gt; Scripting Editor &amp; Monitor, paste this script, click Run
    - Or install as a .xrnx tool (see manifest.xml)

  Configure the two paths at the top of this file.
--]]

-- ================= CONFIGURE THESE =================
local SNAPSHOT_PATH  = "/tmp/tal-pha-lush-pad.txt"   -- snapshot file to read
local VSTPARAM_HOST  = "/home/baboon/bin/vstparam_host"  -- path to vstparam_host binary
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
-- Main
--------------------------------------------------------------------------------
local function apply_snapshot()
  local song = renoise.song()

  -- 1. Prompt user to select a snapshot file
  local snapshot_path = nil

  -- Try zenity first (Linux native file dialog)
  local ok, result = pcall(function()
    local h = io.popen(
      "zenity --file-selection --title='Select VST snapshot file' " ..
      "--file-filter='Snapshot files (*.txt *.preset *.snapshot)' " ..
      "--filename=" .. shellescape(SNAPSHOT_PATH) .. " 2>/dev/null",
      "r"
    )
    if h then
      local r = trim(h:read("*a"))
      h:close()
      return r
    end
    return nil
  end)
  if ok and result and result ~= "" then
    snapshot_path = result
  end

  -- Fallback: text input dialog (cross-platform)
  if not snapshot_path then
    local vb = renoise.ViewBuilder()
    local button = renoise.app():show_custom_prompt(
      "VST Snapshot Applier",
      {
        vb:text { text = "Enter snapshot file path:" },
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
  end

  if not snapshot_path then
    renoise.app():show_prompt(
      "VST Snapshot Applier",
      "No snapshot file selected.",
      {"OK"}
    )
    return
  end

  -- 2. Find the first VST instrument in the song (just for its name)
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

  -- 2. Get plugin path from user (Renoise does not expose plugin file path)
  local plugin_path = nil

  local vb = renoise.ViewBuilder()
  local button = renoise.app():show_custom_prompt(
    "VST Snapshot Applier — VST Path",
    {
      vb:text {
        text = "Enter the .vst3 bundle path for '" .. tostring(target_instr.name) .. "':",
      },
      vb:textfield {
        id = "plugin_path",
        text = "/home/baboon/.vst3/" .. tostring(target_instr.name) .. ".vst3",
      },
    },
    {"OK", "Cancel"}
  )
  if button == "OK" then
    local entered = trim(vb.views.plugin_path.text)
    if entered and entered ~= "" then
      plugin_path = entered
    end
  end

  if not plugin_path then
    renoise.app():show_prompt(
      "VST Snapshot Applier",
      "No VST path entered.",
      {"OK"}
    )
    return
  end

  -- 3. Parse snapshot
  local entries, err = parse_snapshot(snapshot_path)
  if not entries then
    renoise.app():show_prompt("VST Snapshot Applier", err, {"OK"})
    return
  end

  -- 4. Get param ID -&gt; title mapping from vstparam_host
  local param_map, err2 = get_vst_param_map(plugin_path)
  if not param_map then
    renoise.app():show_prompt("VST Snapshot Applier", err2, {"OK"})
    return
  end

  -- 5. Build Renoise name -&gt; 1-based index map
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
    "Applied %d / %d parameters to instrument '%s'\nSkipped: %d\n\n%s",
    applied,
    #entries,
    tostring(target_instr.name),
    skipped,
    table.concat(results, "\n"):sub(1, 3000)
  )

  renoise.app():show_prompt("VST Snapshot Applier — Done", msg, {"OK"})
end

renoise.tool():add_menu_entry {
  name = "Main Menu:Tools:VST Snapshot Applier",
  invoke = apply_snapshot
}
