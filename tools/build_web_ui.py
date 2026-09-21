#!/usr/bin/env python3
"""Generate browser and firmware UI artifacts from web/index.html."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "web" / "index.html"
DOCS = ROOT / "docs" / "index.html"
HEADER = ROOT / "firmware" / "embedded_ui.h"
RAW_TERMINATOR = ")AMB_CC_HTML"

def main() -> None:
    html = SOURCE.read_text(encoding="utf-8")
    if RAW_TERMINATOR in html:
        raise SystemExit(
            'ERROR: raw R"AMB_CC_HTML terminator found in web/index.html'
        )

    DOCS.write_text(html, encoding="utf-8")
    HEADER.write_text(
        "#pragma once\n"
        "#include <Arduino.h>\n\n"
        "// GENERATED FILE — DO NOT EDIT.\n"
        "// Source of truth: web/index.html\n"
        "static const char AMBILIGHT_CC_HTML[] PROGMEM = R\"AMB_CC_HTML(\n"
        + html +
        ")AMB_CC_HTML\";\n",
        encoding="utf-8",
    )
    print(f"OK docs/index.html: {len(html)} bytes")
    print(f"OK firmware/embedded_ui.h: {len(html)} HTML bytes")

if __name__ == "__main__":
    main()
