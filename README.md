# xways-json-viewer

A viewer X-Tension for **X-Ways Forensics**: a live **JMESPath** query box over
JSON and JSONL files, rendered inside the Preview pane.

> **Status: 0.1.0-beta** — works on X-Ways Forensics 21.8; interfaces may still
> change before the first non-beta release.

## What it does

- Claims JSON-ish files in Preview mode by X-Ways type (`JavaScript Object
  Notation`), by extension (`.json .jsonl .ndjson .geojson .har .ipynb .jsonc
  .webmanifest .json5 .topojson .jsonld .avsc`), or — for text-typed items — by
  a leading `{` / `[`. Everything else is left to the next viewer.
- Shows the file in an embedded **Edge WebView2** page:
  - **JMESPath query** ([jmespath.org](https://jmespath.org/)) that re-runs as
    you type; errors are shown inline; empty query = whole document. The query
    is kept when you move to the next file, so one expression can be checked
    against many files.
  - **Tree** view (collapsible, lazy, large arrays chunked) and **Text** view
    (pretty-printed, selectable for copy).
  - **JSONL** mode (auto-detected for line-delimited files): each line becomes
    a record and the document an array, so filters like
    `` [?status == `500`].url `` work over logs.
  - **History** (▾): queries that returned data are saved automatically, per
    file, and can be re-applied with a click. Stored in
    `xways-json-viewer.cfg` next to the DLL (`history=<file>\t<query>`).
  - **Render cap** (8 MiB default, up to 512 MiB): how much of a file is read
    into the viewer; applies from the next selection.
  - `?` opens a JMESPath cheat-sheet.
- Encoding sniff: UTF-8 / UTF-16 LE / UTF-16 BE BOMs, strict UTF-8, Windows-1252
  fallback.

## Requirements

- Windows x64, **X-Ways Forensics 21.x or newer** with a forensic licence
  (viewer X-Tensions are not available in WinHex Lab Edition).
- **Options → Viewer Programs → "Activate separate viewer component" must be
  ON.** Without it X-Ways still calls the X-Tension but never creates a preview
  window to host in, and shows its rudimentary ASCII preview instead.
- **Microsoft Edge WebView2 Runtime** (preinstalled on Windows 11 and on
  Windows 10 with Edge; otherwise the Evergreen installer from Microsoft). The
  Messages window says so at load if it is missing.

## Install

```text
<X-Ways install>\
    └── xtensions\
        └── xways-json-viewer\
            └── xways-json-viewer.dll
```

Viewer X-Tensions are **not** registered under Tools → Run X-Tensions. Use
**Options → Viewer Programs → File Viewing → tick "Load viewer X-Tensions" → "…"
→ "+"** and pick the DLL. Only the first viewer X-Tension in that list that
claims a file renders it, so order matters if you load several.

No sidecar files: the page and the JMESPath engine are embedded in the DLL.
WebView2 keeps its profile under `%LOCALAPPDATA%\xways-json-viewer\WebView2`
(delete that folder to remove all local state).

## Build

From a "x64 Native Tools Command Prompt" (or plain cmd — `build.bat` bootstraps
MSVC): `build.bat`. Output: `xtensions\xways-json-viewer\xways-json-viewer.dll`.

One-time dependency (gitignored): download the `Microsoft.Web.WebView2` NuGet
package (<https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2> — it is
a zip) and unzip `build/native/include/WebView2.h` and
`build/native/x64/WebView2LoaderStatic.lib` into `vendor/webview2/build/native/`.
Tested with SDK 1.0.4129.50 and Runtime 151.

## How it works

Returned HTML cannot run script in the X-Ways viewer component, so the
X-Tension returns a 1-byte buffer (nothing to draw) and parents a WebView2
control onto the viewer's preview window (`XWF_GetWindow(0, 6)`). X-Ways
creates that window lazily and destroys it on mode switches and same-file
re-selection, so the X-Tension re-attaches from a short retry timer, keeps
itself sized and on top with a small watchdog (the viewer window forwards no
size messages, and Outside In's own child paints over anything below it), and
recreates the WebView2 controller when the window goes away. The file's bytes
are posted to the page as a JSON message; the page does the parsing and
querying.

## Security posture

Evidence bytes are treated as hostile data, never as code: they only pass
through `JSON.parse` and are rendered as text. Following Microsoft's WebView2
guidance, the host disables host objects, script dialogs, autofill / password
save and external file drop; cancels every navigation away from the embedded
page; blocks popups; and trims the right-click menu to copy / cut / paste /
select-all / print. DevTools (Inspect) are enabled only in verbose builds
(`VERBOSE = true` in the source).

## Third-party

- `ui/jmespath.js` — [jmespath.js](https://github.com/jmespath/jmespath.js),
  Apache License 2.0 (see `ui/jmespath.LICENSE`). Embedded unmodified.
- WebView2 SDK — Microsoft, per its NuGet licence (not redistributed here).

## Roadmap

- [ ] Search / highlight inside the tree; copy-path on a node.
- [ ] `.har` / `.ipynb` presets (pre-filled queries).
- [ ] Per-row delete in the history panel.
- [ ] Verify behaviour with two data windows open.

## Disclaimer

Community-developed X-Tension. **Not** affiliated with, endorsed by, or
supported by X-Ways AG. Use at your own risk; please open an issue if you find a
bug.

## License

MIT — see [LICENSE](LICENSE).
