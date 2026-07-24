#!/usr/bin/env python3
"""Build the Waycast site: inline all assets into self-contained pages.

  src/en.html       -> index.html
  src/hu.html       -> index.hu.html
  src/<page>.html   -> <page>.html        (all other pages)

Assets ({{PHOTO}}, {{POSTER}}, {{VIDEO}}, {{PROTO}}) become data URIs
so each page is one file you can host anywhere or attach to an email.
"""
import base64
import pathlib

HERE = pathlib.Path(__file__).resolve().parent

def uri(name, mime):
    return f"data:{mime};base64," + base64.b64encode(
        (HERE / name).read_bytes()).decode()

ASSETS = {
    "{{PHOTO}}": uri("waycast-ui.jpg", "image/jpeg"),
    "{{POSTER}}": uri("waycast-poster.jpg", "image/jpeg"),
    "{{VIDEO}}": uri("waycast-demo.mp4", "video/mp4"),
    "{{DEVDRIVE}}": uri("waycast-device-drive.jpg", "image/jpeg"),
    "{{DEVSQUARE}}": uri("waycast-device-square.jpg", "image/jpeg"),
}

RENAME = {"en.html": "index.html", "hu.html": "index.hu.html"}

AIRJS = (HERE / "src" / "_air.js").read_text()

for src in sorted((HERE / "src").glob("*.html")):
    out = RENAME.get(src.name, src.name)
    html = src.read_text()
    html = html.replace("{{AIRJS}}", AIRJS)
    for k, v in ASSETS.items():
        html = html.replace(k, v)
    (HERE / out).write_text(html)
    print(f"{out}: {len(html)//1024} KB")
