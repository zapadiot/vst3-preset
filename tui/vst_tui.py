#!/usr/bin/env python3
"""vst_tui — Textual TUI for vstparam_host.

Lists VST3 parameters, lets you set/save/load snapshots, and includes a
prompt/research area plus a preview placeholder for MIDI replay.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
from typing import Optional

from textual.app import App, ComposeResult
from textual.containers import Container, Vertical, ScrollableContainer
from textual.widgets import Button, DataTable, Footer, Header, Input, Label, Static
from textual.binding import Binding


VSTPARAM_HOST = os.environ.get("VSTPARAM_HOST", "vstparam_host")
DEFAULT_SNAPSHOT = "/tmp/vst-snapshot.txt"


class VstParamTui(App[None]):
    CSS = """
    Screen {
        layout: vertical;
    }
    #header-bar {
        height: 3;
        dock: top;
        padding: 0 1;
    }
    #main {
        height: 1fr;
        layout: horizontal;
    }
    #params-panel {
        width: 1fr;
        height: 1fr;
        border: solid green;
        padding: 0 1;
    }
    #right-panel {
        width: 62;
        height: 1fr;
        layout: vertical;
        border: solid blue;
        padding: 0 1;
    }
    #prompt-area {
        height: 1fr;
        border: solid yellow;
        padding: 0 1;
    }
    #preview-area {
        height: 1fr;
        border: solid magenta;
        padding: 0 1;
    }
    #set-panel {
        height: 7;
        dock: bottom;
        layout: horizontal;
        border: solid cyan;
        padding: 0 1;
    }
    .panel-title {
        text-style: bold;
        margin-bottom: 1;
    }
    .result {
        height: 1fr;
        overflow: auto;
    }
    """

    BINDINGS = [
        Binding("q", "quit", "Quit"),
        Binding("r", "refresh_params", "Refresh"),
        Binding("s", "save_snapshot", "Save snapshot"),
        Binding("l", "load_snapshot", "Load snapshot"),
    ]

    def __init__(self, plugin_path: str):
        super().__init__()
        self.plugin_path = plugin_path
        self.parameters = []  # list of dicts

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        with Container(id="header-bar"):
            yield Label(f"Plugin: {self.plugin_path}", id="plugin-label")

        with Container(id="main"):
            with Vertical(id="params-panel"):
                yield Label("Parameters", classes="panel-title")
                yield DataTable(id="params-table", zebra_stripes=True, cursor_type="row")

            with Vertical(id="right-panel"):
                with Vertical(id="prompt-area"):
                    yield Label("Prompt / preset research", classes="panel-title")
                    yield Input(
                        placeholder="e.g. make this warmer / brighter ...",
                        id="prompt-input",
                    )
                    yield Button(
                        "Apply research notes",
                        id="apply-prompt",
                        variant="primary",
                    )
                    yield Static(id="prompt-result", classes="result")

                with Vertical(id="preview-area"):
                    yield Label(
                        "Preview / MIDI replay (placeholder)", classes="panel-title"
                    )
                    yield Static(id="preview-content", classes="result")
                    yield Button(
                        "Play test note (needs synth)",
                        id="play-note",
                        variant="default",
                    )

        with Container(id="set-panel"):
            yield Label("Set parameter", classes="panel-title")
            yield Input(placeholder="param ID (decimal or 0x hex)", id="set-id")
            yield Input(placeholder="normalized value 0..1", id="set-value")
            yield Button("Set", id="set-param", variant="success")
            yield Static(id="set-result", classes="result")

        yield Footer()

    def on_mount(self) -> None:
        table = self.query_one("#params-table", DataTable)
        table.add_columns(
            "Index", "ID (hex)", "Title", "Default", "Value", "Units", "Flags"
        )
        self.refresh_params()

    # ------------------------------------------------------------------ #
    # Parameter loading / parsing
    # ------------------------------------------------------------------ #
    def refresh_params(self) -> None:
        """Run vstparam_host and populate the table."""
        try:
            proc = subprocess.run(
                [VSTPARAM_HOST, self.plugin_path],
                capture_output=True,
                text=True,
                timeout=60,
            )
        except FileNotFoundError:
            self.notify(
                f"vstparam_host not found ({VSTPARAM_HOST})", severity="error"
            )
            return
        except subprocess.TimeoutExpired:
            self.notify("vstparam_host timed out", severity="error")
            return

        self.parameters = self._parse_output(proc.stdout)
        table = self.query_one("#params-table", DataTable)
        table.clear()
        for p in self.parameters:
            table.add_row(
                str(p["index"]),
                p["id_hex"],
                p["title"],
                str(p["default"]),
                str(p["value"]),
                p["units"],
                str(p["flags"]),
            )
        self.notify(f"Loaded {len(self.parameters)} parameters")

    @staticmethod
    def _parse_output(stdout: str) -> list[dict]:
        params = []
        line_re = re.compile(
            r"^\[\s*(?P<index>\d+)\]\s+id=(?P<id>0x[0-9a-fA-F]+|\d+)\s+(?P<title>.*?)\s+def=(?P<def>\S+)\s+value=(?P<value>\S+)\s+units=(?P<units>.*?)\s+flags=(?P<flags>\S+)\s*$"
        )
        for line in stdout.splitlines():
            m = line_re.match(line)
            if not m:
                continue
            d = m.groupdict()
            id_str = d["id"]
            try:
                id_int = int(id_str, 0)
            except Exception:
                id_int = None
            id_hex = (
                id_str
                if id_str.lower().startswith("0x")
                else (hex(id_int) if id_int is not None else id_str)
            )

            def to_float(s: str):
                try:
                    return float(s)
                except Exception:
                    return s

            params.append(
                {
                    "index": int(d["index"]) if d["index"].isdigit() else None,
                    "id_hex": id_hex,
                    "id_decimal": id_int,
                    "title": d["title"],
                    "default": to_float(d["def"]),
                    "value": to_float(d["value"]),
                    "units": d["units"],
                    "flags": d["flags"],
                }
            )
        return params

    # ------------------------------------------------------------------ #
    # Actions
    # ------------------------------------------------------------------ #
    def action_refresh_params(self) -> None:
        self.refresh_params()

    def action_save_snapshot(self) -> None:
        proc = subprocess.run(
            [VSTPARAM_HOST, self.plugin_path, "--save", DEFAULT_SNAPSHOT],
            capture_output=True,
            text=True,
            timeout=60,
        )
        msg = proc.stdout.strip() or proc.stderr.strip()
        self.notify(
            msg,
            severity="info" if proc.returncode == 0 else "error",
            timeout=5,
        )

    def action_load_snapshot(self) -> None:
        proc = subprocess.run(
            [VSTPARAM_HOST, self.plugin_path, "--load", DEFAULT_SNAPSHOT],
            capture_output=True,
            text=True,
            timeout=60,
        )
        msg = proc.stdout.strip() or proc.stderr.strip()
        self.notify(
            msg,
            severity="info" if proc.returncode == 0 else "error",
            timeout=5,
        )
        self.refresh_params()

    # ------------------------------------------------------------------ #
    # Button handlers
    # ------------------------------------------------------------------ #
    def on_button_pressed(self, event: Button.Pressed) -> None:
        bid = event.button.id
        if bid == "set-param":
            self._do_set_param()
        elif bid == "apply-prompt":
            self._do_apply_prompt()
        elif bid == "play-note":
            self._do_play_note()

    def _do_set_param(self) -> None:
        pid = self.query_one("#set-id", Input).value.strip()
        val = self.query_one("#set-value", Input).value.strip()
        result = self.query_one("#set-result", Static)
        if not pid or not val:
            result.update("Enter both parameter ID and value.")
            return
        proc = subprocess.run(
            [VSTPARAM_HOST, self.plugin_path, "--set", pid, val],
            capture_output=True,
            text=True,
            timeout=60,
        )
        out = proc.stdout.strip() or proc.stderr.strip()
        result.update(out)
        self.notify(
            f"set {pid}={val} -> {'ok' if proc.returncode == 0 else 'failed'}",
            severity="info" if proc.returncode == 0 else "error",
            timeout=4,
        )
        if proc.returncode == 0:
            self.refresh_params()

    def _do_apply_prompt(self) -> None:
        prompt = self.query_one("#prompt-input", Input).value.strip()
        result = self.query_one("#prompt-result", Static)
        if not prompt:
            result.update("Enter a prompt first.")
            return
        result.update(
            f"Research prompt:\n{prompt}\n\n"
            "(Wire web_search/web_fetch from vstpreset-mcp here to turn this "
            "into parameter values.)"
        )
        self.notify("Prompt captured — research integration pending", timeout=4)

    def _do_play_note(self) -> None:
        preview = self.query_one("#preview-content", Static)
        preview.update(
            "MIDI replay is a placeholder.\n"
            "Needs a synth backend (FluidSynth / Timidity / software synth)\n"
            "plus a MIDI output port wired to this plugin.\n\n"
            "Not implemented yet — preview area ready for wiring."
        )
        self.notify("MIDI replay needs a synth backend", timeout=4)


def main() -> None:
    plugin = sys.argv[1] if len(sys.argv) > 1 else "/home/baboon/.vst3/TAL-EQ.vst3"
    app = VstParamTui(plugin)
    app.run()


if __name__ == "__main__":
    main()
