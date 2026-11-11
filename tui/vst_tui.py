#!/usr/bin/env python3
"""vst_tui — Textual TUI for vstparam_host.

Lists VST3 parameters, lets you set/save/load snapshots, and wires a
prompt/research area that searches the web for preset tutorials and lets
you apply discovered parameter values.

Preview / audio: vstparam_host is a parameter-control host (no audio
rendering engine). To actually *hear* the plugin, load it in a DAW/audio
host (Carla, Bitwig, Ardour, …) and use this TUI to tweak parameters in
real time.
"""

from __future__ import annotations

import html
import os
import re
import subprocess
import sys
import urllib.parse
import urllib.request
from typing import Optional

from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Container, Vertical, ScrollableContainer
from textual.widgets import Button, DataTable, Footer, Header, Input, Label, Static


VSTPARAM_HOST = os.environ.get("VSTPARAM_HOST", "vstparam_host")
DEFAULT_SNAPSHOT = "/tmp/vst-snapshot.txt"
USER_AGENT = "Mozilla/5.0 (X11; Linux x86_64) vst-tui/0.1"


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
        height: 10;
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
    .muted {
        color: $text-muted;
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
                        placeholder='e.g. "warm vocal EQ settings"',
                        id="prompt-input",
                    )
                    yield Button(
                        "Research preset",
                        id="research-preset",
                        variant="primary",
                    )
                    yield Static(id="research-result", classes="result")

                with Vertical(id="preview-area"):
                    yield Label(
                        "Preview / MIDI replay (placeholder)", classes="panel-title"
                    )
                    yield Static(
                        "Audio preview is not available from this host:\n"
                        "vstparam_host controls parameters only; it does not\n"
                        "render audio. Load the plugin in a DAW (Carla,\n"
                        "Bitwig, Ardour…) and tweak parameters here.\n",
                        id="preview-content",
                        classes="result muted",
                    )
                    yield Button(
                        "Open plugin in DAW (note only)",
                        id="daw-note",
                        variant="default",
                    )

        with Container(id="set-panel"):
            yield Label("Set parameter", classes="panel-title")
            yield Input(placeholder="param ID (decimal or 0x hex)", id="set-id")
            yield Input(placeholder="normalized value 0..1", id="set-value")
            yield Button("Set", id="set-param", variant="success")
            yield Button("Save snapshot", id="save-snapshot", variant="default")
            yield Button("Load snapshot", id="load-snapshot", variant="default")
            yield Static(id="set-result", classes="result")
            yield Label("Apply research overrides", classes="panel-title")
            yield Input(
                placeholder="id=value,id=value (e.g. 1336600346=0.75,0x1b09d=0.4)",
                id="override-input",
            )
            yield Button("Apply overrides", id="apply-overrides", variant="default")
            yield Static(id="override-result", classes="result")

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
    # Web research (prompt wiring)
    # ------------------------------------------------------------------ #
    def _fetch_url(self, url: str, timeout: int = 20, max_bytes: int = 2_000_000):
        req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            ctype = resp.headers.get("Content-Type", "")
            raw = resp.read(max_bytes + 1)
            truncated = len(raw) > max_bytes
            if truncated:
                raw = raw[:max_bytes]
            charset = "utf-8"
            m = re.search(r"charset=([^\s;]+)", ctype, re.I)
            if m:
                charset = m.group(1).strip("'\" ")
            try:
                text = raw.decode(charset, errors="replace")
            except Exception:
                text = raw.decode("utf-8", errors="replace")
        return text, (" (truncated)" if truncated else "")

    def _extract_text(self, html_text: str) -> str:
        # crude strip of tags + whitespace collapse
        text = re.sub(r"<script[^>]*>.*?</script>", " ", html_text, flags=re.I | re.S)
        text = re.sub(r"<style[^>]*>.*?</style>", " ", text, flags=re.I | re.S)
        text = re.sub(r"<[^>]+>", " ", text)
        text = html.unescape(text)
        text = re.sub(r"\s+", " ", text)
        return text.strip()

    def _ddg_search(self, query: str, max_results: int = 8) -> list[dict]:
        url = "https://html.duckduckgo.com/html/?" + urllib.parse.urlencode({"q": query})
        try:
            text, _ = self._fetch_url(url, timeout=20)
        except Exception:
            return []

        # DuckDuckGo often serves an anomaly/CAPTCHA page to headless clients.
        if "anomaly-modal" in text or "challenge-form" in text:
            raise RuntimeError("DuckDuckGo CAPTCHA/anomaly page — direct scraping blocked")

        results = []
        for m in re.finditer(
            r'<a[^>]+class="[^"]*result__a[^"]*"[^>]+href="([^"]+)"[^>]*>(.*?)</a>',
            text,
            re.I | re.S,
        ):
            href = html.unescape(m.group(1))
            title = re.sub(r"<[^>]+>", "", m.group(2))
            title = html.unescape(title).strip()
            if "uddg=" in href:
                try:
                    parsed = urllib.parse.urlparse(href)
                    qs = urllib.parse.parse_qs(parsed.query)
                    if "uddg" in qs:
                        href = qs["uddg"][0]
                except Exception:
                    pass
            results.append({"title": title, "url": href})
            if len(results) >= max_results:
                break

        snippets = re.findall(
            r'class="[^"]*result__snippet[^"]*"[^>]*>(.*?)</a>', text, re.I | re.S
        )
        for i, sn in enumerate(snippets[: len(results)]):
            results[i]["snippet"] = html.unescape(
                re.sub(r"<[^>]+>", "", sn)
            ).strip()
        return results

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
        elif bid == "save-snapshot":
            self.action_save_snapshot()
        elif bid == "load-snapshot":
            self.action_load_snapshot()
        elif bid == "research-preset":
            self._do_research_preset()
        elif bid == "apply-overrides":
            self._do_apply_overrides()
        elif bid == "daw-note":
            self._do_daw_note()

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

    def _do_research_preset(self) -> None:
        prompt = self.query_one("#prompt-input", Input).value.strip()
        result = self.query_one("#research-result", Static)
        if not prompt:
            result.update("Enter a prompt first.")
            return

        result.update("Searching…")
        try:
            results = self._ddg_search(prompt, max_results=6)
        except RuntimeError as e:
            result.update(
                f"Search blocked: {e}\n\n"
                "Workaround: research in a browser, then paste findings into\n"
                "'Apply research overrides' as id=value,id=value and click Apply."
            )
            return
        except Exception as e:
            result.update(f"Search error: {e}")
            return

        if not results:
            result.update("No results found.")
            return

        lines = [f"Research: {prompt}\n"]
        for i, r in enumerate(results, 1):
            lines.append(f"{i}. {r['title']}")
            lines.append(f"   {r['url']}")
            if r.get("snippet"):
                lines.append(f"   {r['snippet'][:200]}")
            lines.append("")
        result.update("\n".join(lines))
        self.notify(f"Found {len(results)} results", timeout=4)

    def _do_apply_overrides(self) -> None:
        text = self.query_one("#override-input", Input).value.strip()
        result = self.query_one("#override-result", Static)
        if not text:
            result.update("Enter overrides like 1336600346=0.75,0x1b09d=0.4")
            return

        pairs = []
        for pair in text.split(","):
            pair = pair.strip()
            if not pair or "=" not in pair:
                continue
            pid, val = pair.split("=", 1)
            pairs.append((pid.strip(), val.strip()))

        if not pairs:
            result.update("No valid overrides found.")
            return

        # Apply all overrides in a single host invocation so they accumulate
        # in one process (each fresh --set call would reload defaults).
        cmd = [VSTPARAM_HOST, self.plugin_path]
        for pid, val in pairs:
            cmd += ["--set", pid, val]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)

        out_lines = proc.stdout.strip().splitlines() if proc.stdout else []
        applied = sum(1 for ln in out_lines if "set ->" in ln and "ret=0" in ln)
        failed = len(pairs) - applied
        if proc.returncode != 0:
            failed = len(pairs)

        result.update(
            proc.stdout.strip()
            or proc.stderr.strip()
            or f"Applied {applied} of {len(pairs)} override(s)"
        )
        self.notify(
            f"Applied {applied} of {len(pairs)} override(s)"
            + (f", {failed} failed" if failed else ""),
            timeout=4,
        )
        if applied:
            self.refresh_params()

    def _do_daw_note(self) -> None:
        preview = self.query_one("#preview-content", Static)
        preview.update(
            "To hear audio:\n"
            "1. Load this plugin in a DAW/audio host (Carla, Bitwig, Ardour…)\n"
            "2. Keep this TUI open and adjust parameters with --set\n"
            "3. Parameter changes apply live to the loaded plugin instance\n\n"
            "vstparam_host itself has no audio rendering engine.\n"
        )
        self.notify("Preview is parameter-only; use a DAW for audio", timeout=4)


def main() -> None:
    plugin = sys.argv[1] if len(sys.argv) > 1 else "/home/baboon/.vst3/TAL-EQ.vst3"
    app = VstParamTui(plugin)
    app.run()


if __name__ == "__main__":
    main()
