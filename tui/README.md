# vst-tui — Textual TUI for vstparam_host

A terminal UI to browse, set, and snapshot VST3 parameters (native Linux and
**vstbridge**-bridged Wine VST3s), with a prompt/research area and a preview
placeholder for MIDI replay.

## Run

```bash
cd tui
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt

python vst_tui.py /home/baboon/.vst3/TAL-EQ.vst3
# or any bundle, incl. vstbridge:
python vst_tui.py "/home/baboon/.vst3/vstbridge/Jup-8 V4.vst3"
```

## Controls

| Key | Action |
|---|---|
| `q` | Quit |
| `r` | Refresh parameter list |
| `s` | Save snapshot to `/tmp/vst-snapshot.txt` |
| `l` | Load snapshot from `/tmp/vst-snapshot.txt` |

## Panels

- **Parameters** — table from `vstparam_host` (index, id, title, default, value, units, flags).
- **Set parameter** — type a parameter ID (decimal or `0x` hex) and normalized value `[0..1]`, then press **Set**.
- **Prompt / preset research** — type a natural-language request (e.g. "make this warmer") and press **Apply research notes**. Research-to-parameter mapping is a placeholder; wire `web_search`/`web_fetch` from `vstpreset-mcp` here.
- **Preview / MIDI replay** — placeholder. Real MIDI replay needs a synth backend (FluidSynth/Timidity + a MIDI output port). Not wired yet.

## Notes

- `vstparam_host` must be on `PATH` or set `VSTPARAM_HOST` to its absolute path.
- Pass bundle paths without a trailing slash (SDK path-resolution quirk).
- Parameter values are normalized `[0..1]` (VST3 controller semantics).
