import html
import json
import logging
import os
import re
import subprocess
import sys
import urllib.parse
import urllib.request
from html.parser import HTMLParser
from pathlib import Path
from typing import Any, Optional

# Keep stdout clean for MCP protocol messages
real_stdout = sys.stdout
sys.stdout = sys.stderr

from mcp.server.mcpserver import MCPServer  # noqa: E402

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
    stream=sys.stderr,
)
logger = logging.getLogger("vstpreset-mcp")

sys.stdout = real_stdout
sys.stdout.reconfigure(line_buffering=True)

mcp = MCPServer("vstpreset")

VSTPARAM_HOST = os.environ.get("VSTPARAM_HOST", "/home/baboon/bin/vstparam_host")
USER_AGENT = "Mozilla/5.0 (X11; Linux x86_64) vstpreset-mcp/0.1"


def _run_host(args: list[str], timeout: int = 300) -> dict[str, Any]:
    cmd = [VSTPARAM_HOST] + args
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except FileNotFoundError:
        return {
            "ok": False,
            "error": f"vstparam_host not found: {VSTPARAM_HOST}",
            "cmd": cmd,
        }
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "command timed out", "cmd": cmd, "timeout": timeout}
    return {
        "ok": proc.returncode == 0,
        "returncode": proc.returncode,
        "cmd": cmd,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
    }


def _parse_param_lines(stdout: str) -> list[dict[str, Any]]:
    params: list[dict[str, Any]] = []
    line_re = re.compile(
        r"^\[\s*(?P<index>\d+)\]\s+id=(?P<id>0x[0-9a-fA-F]+|\d+)\s+(?P<title>.*?)\s+def=(?P<def>\S+)\s+value=(?P<value>\S+)\s+units=(?P<units>.*?)\s+flags=(?P<flags>\S+)\s*$"
    )
    for line in stdout.splitlines():
        m = line_re.match(line)
        if not m:
            continue
        d = m.groupdict()
        try:
            idx = int(d["index"])
        except Exception:
            idx = None
        id_str = d["id"]
        try:
            id_int = int(id_str, 0)
        except Exception:
            id_int = None
        def _to_float(s: str) -> Any:
            try:
                return float(s)
            except Exception:
                return s
        params.append(
            {
                "index": idx,
                "id_hex": id_str if id_str.lower().startswith("0x") else (hex(id_int) if id_int is not None else id_str),
                "id_decimal": id_int,
                "title": d["title"],
                "default": _to_float(d["def"]),
                "value": _to_float(d["value"]),
                "units": d["units"],
                "flags": d["flags"],
            }
        )
    return params


class _TextExtractor(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self._parts: list[str] = []
        self._skip = 0
        self._skip_tags = {"script", "style", "noscript", "svg", "header", "footer", "nav"}

    def handle_starttag(self, tag: str, attrs: list[tuple[str, Optional[str]]]) -> None:
        if tag in self._skip_tags:
            self._skip += 1
        if tag in {"br", "p", "div", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6"}:
            self._parts.append("\n")

    def handle_endtag(self, tag: str) -> None:
        if tag in self._skip_tags and self._skip > 0:
            self._skip -= 1
        if tag in {"p", "div", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6"}:
            self._parts.append("\n")

    def handle_data(self, data: str) -> None:
        if self._skip == 0:
            self._parts.append(data)

    def get_text(self) -> str:
        text = "".join(self._parts)
        text = html.unescape(text)
        # collapse whitespace but keep paragraph breaks
        text = re.sub(r"[ \t\r\f\v]+", " ", text)
        text = re.sub(r"\n[ \t]+", "\n", text)
        text = re.sub(r"\n{3,}", "\n\n", text)
        return text.strip()


def _fetch_url(url: str, timeout: int = 20, max_bytes: int = 2_000_000) -> tuple[str, str]:
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
            charset = m.group(1).strip('"\'')
        try:
            text = raw.decode(charset, errors="replace")
        except Exception:
            text = raw.decode("utf-8", errors="replace")
    return text, (" (truncated)" if truncated else "")


@mcp.tool()
async def vst_list_parameters(path: str) -> dict:
    """List all parameters of a VST3 bundle (native or vstbridge-bridged).

    Args:
        path: Absolute path to the .vst3 bundle directory.
    """
    logger.info("Listing parameters for %s", path)
    res = _run_host([path])
    params = _parse_param_lines(res.get("stdout", "")) if res.get("stdout") else []
    plug_line = ""
    for line in (res.get("stdout") or "").splitlines():
        if line.startswith("plug-in:"):
            plug_line = line
            break
    res["plugin"] = plug_line
    res["parameter_count"] = len(params)
    res["parameters"] = params
    return res


@mcp.tool()
async def vst_find_parameters(path: str, query: str, limit: int = 200) -> dict:
    """Find parameters whose title contains the query (case-insensitive).

    Args:
        path: Absolute path to the .vst3 bundle directory.
        query: Substring to search for in parameter titles.
        limit: Maximum number of matches to return.
    """
    logger.info("Finding parameters matching '%s' in %s", query, path)
    res = _run_host([path])
    params = _parse_param_lines(res.get("stdout", "")) if res.get("stdout") else []
    q = (query or "").lower()
    matches = [p for p in params if q in (p.get("title") or "").lower()]
    if limit and len(matches) > limit:
        matches = matches[:limit]
    plug_line = ""
    for line in (res.get("stdout") or "").splitlines():
        if line.startswith("plug-in:"):
            plug_line = line
            break
    return {
        "ok": res.get("ok"),
        "returncode": res.get("returncode"),
        "plugin": plug_line,
        "query": query,
        "match_count": len(matches),
        "matches": matches,
        "stderr": res.get("stderr", ""),
    }


@mcp.tool()
async def vst_set_parameter(path: str, param_id: str, value: float) -> dict:
    """Set a VST3 parameter to a normalized value.

    Args:
        path: Absolute path to the .vst3 bundle directory.
        param_id: Parameter ID as decimal or hex (e.g. "1336600346" or "0x4faae71a").
        value: Normalized value, typically in [0..1].
    """
    logger.info("Setting %s=%s for %s", param_id, value, path)
    return _run_host([path, "--set", str(param_id), str(value)])


@mcp.tool()
async def vst_save_snapshot(path: str, snapshot_path: str) -> dict:
    """Save all parameters of a VST3 plugin to a snapshot file.

    Snapshot format:
      # VST3 parameter preset snapshot
      # format: <paramID decimal> <normalizedValue 0..1>

    Args:
        path: Absolute path to the .vst3 bundle directory.
        snapshot_path: Output file path for the snapshot.
    """
    logger.info("Saving snapshot for %s -> %s", path, snapshot_path)
    Path(snapshot_path).parent.mkdir(parents=True, exist_ok=True)
    return _run_host([path, "--save", snapshot_path])


@mcp.tool()
async def vst_load_snapshot(path: str, snapshot_path: str) -> dict:
    """Load a parameter snapshot into a VST3 plugin and list resulting parameter values.

    Args:
        path: Absolute path to the .vst3 bundle directory.
        snapshot_path: Snapshot file path to load.
    """
    logger.info("Loading snapshot %s -> %s", snapshot_path, path)
    return _run_host([path, "--load", snapshot_path])


@mcp.tool()
async def web_search(query: str, max_results: int = 8) -> dict:
    """Search the web for VST/plugin documentation, manuals, and preset tutorials.

    Uses DuckDuckGo HTML results (no API key required). Useful for finding parameter
    meanings, recommended settings, and preset recipes before mapping them to plugin
    parameters.

    Args:
        query: Search query (e.g. "TAL-EQ warm vocal preset settings tutorial").
        max_results: Maximum number of results to return.
    """
    logger.info("Searching web: %s", query)
    url = "https://html.duckduckgo.com/html/?" + urllib.parse.urlencode({"q": query})
    try:
        text, trunc_note = _fetch_url(url, timeout=20)
    except Exception as e:
        return {"ok": False, "error": str(e), "query": query, "url": url}

    results = []
    # DuckDuckGo HTML: results in <a class="result__a" href="...">title</a>
    # and snippets in <a class="result__snippet"> or similar blocks
    for m in re.finditer(
        r'<a[^>]+class="[^"]*result__a[^"]*"[^>]+href="([^"]+)"[^>]*>(.*?)</a>',
        text,
        re.I | re.S,
    ):
        href = html.unescape(m.group(1))
        title = re.sub(r"<[^>]+>", "", m.group(2))
        title = html.unescape(title).strip()
        # unwrap DDG redirect links
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

    # try to attach snippets
    snippets = re.findall(r'class="[^"]*result__snippet[^"]*"[^>]*>(.*?)</a>', text, re.I | re.S)
    for i, sn in enumerate(snippets[: len(results)]):
        sn_clean = re.sub(r"<[^>]+>", "", sn)
        sn_clean = html.unescape(sn_clean).strip()
        results[i]["snippet"] = sn_clean

    return {
        "ok": True,
        "query": query,
        "result_count": len(results),
        "results": results,
        "note": ("results page truncated" + trunc_note).strip() if trunc_note else None,
    }


@mcp.tool()
async def web_fetch(url: str, max_chars: int = 20000) -> dict:
    """Fetch a URL and extract readable text content.

    Useful for reading manuals, tutorials, forum posts, and preset guides found via
    web_search so parameter names/values can be mapped to a VST's parameters.

    Args:
        url: HTTP/HTTPS URL to fetch.
        max_chars: Maximum characters of extracted text to return.
    """
    logger.info("Fetching %s", url)
    try:
        text, trunc_note = _fetch_url(url, timeout=25)
    except Exception as e:
        return {"ok": False, "error": str(e), "url": url}

    # extract title
    title_m = re.search(r"<title[^>]*>(.*?)</title>", text, re.I | re.S)
    title = html.unescape(re.sub(r"\s+", " ", title_m.group(1)).strip()) if title_m else ""

    parser = _TextExtractor()
    try:
        parser.feed(text)
        extracted = parser.get_text()
    except Exception:
        extracted = re.sub(r"<[^>]+>", " ", text)
        extracted = html.unescape(re.sub(r"\s+", " ", extracted)).strip()

    out_trunc = ""
    if len(extracted) > max_chars:
        extracted = extracted[:max_chars]
        out_trunc = f" (truncated to {max_chars} chars)"

    return {
        "ok": True,
        "url": url,
        "title": title,
        "text": extracted,
        "note": (trunc_note + out_trunc).strip() or None,
    }


def main() -> None:
    logger.info("Starting vstpreset-mcp (vstparam_host=%s)", VSTPARAM_HOST)
    mcp.run()


if __name__ == "__main__":
    main()
