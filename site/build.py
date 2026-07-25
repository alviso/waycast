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

# ---- wiki: repo /wiki/*.md -> site /wiki/*.html (same palette) ----
import markdown

WIKI_SRC = HERE.parent / "wiki"
WIKI_OUT = HERE / "wiki"
WIKI_OUT.mkdir(exist_ok=True)

WIKI_TMPL = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title} — Waycast</title>
<meta name="description" content="Step-by-step Waycast build guides — car device, town node, workstation.">
<style>
:root{{--ground:#101318;--panel:#171B22;--line:#262C36;--text:#E7ECF2;
--muted:#8B95A3;--dim:#5C6672;--signal:#F59F00}}
*{{box-sizing:border-box}}body{{margin:0;background:var(--ground);
color:var(--text);font:16px/1.65 -apple-system,"Segoe UI",Roboto,sans-serif}}
nav{{border-bottom:1px solid var(--line);padding:14px 0}}
nav .wrap,main{{max-width:760px;margin:0 auto;padding:0 20px}}
nav a{{color:var(--muted);text-decoration:none;margin-right:18px}}
nav a:hover{{color:var(--text)}}
nav .brand{{font-weight:800;letter-spacing:.06em;color:var(--text)}}
nav .brand span{{color:var(--signal)}}
main{{padding:32px 20px 64px}}
h1{{font-size:32px;line-height:1.2}}h2{{margin-top:40px;font-size:22px}}
a{{color:var(--signal);text-decoration:none}}a:hover{{text-decoration:underline}}
code{{background:var(--panel);border:1px solid var(--line);border-radius:4px;
padding:1px 5px;font-size:14px}}
pre{{background:var(--panel);border:1px solid var(--line);border-radius:8px;
padding:14px;overflow-x:auto}}pre code{{border:0;background:none;padding:0}}
table{{border-collapse:collapse;width:100%;margin:16px 0}}
th,td{{border:1px solid var(--line);padding:8px 10px;text-align:left;
font-size:15px}}th{{color:var(--muted)}}
blockquote{{border-left:3px solid var(--signal);margin:16px 0;
padding:2px 16px;color:var(--muted)}}
footer{{border-top:1px solid var(--line);padding:24px 20px;color:var(--dim);
text-align:center;font-size:14px}}
</style></head><body>
<nav><div class="wrap">
<a class="brand" href="/">WAY<span>CAST</span></a>
<a href="/wiki/">Build guides</a>
<a href="https://github.com/alviso/waycast">GitHub</a>
<a href="https://www.reddit.com/r/waycast/">r/waycast</a>
</div></nav>
<main>{body}</main>
<footer>WAYCAST &middot; open firmware (GPLv3) &middot;
map data &copy; OpenStreetMap contributors</footer>
</body></html>"""

for md_file in sorted(WIKI_SRC.glob("*.md")):
    text = md_file.read_text()
    title = text.splitlines()[0].lstrip("# ").strip()
    body = markdown.markdown(
        text, extensions=["tables", "fenced_code", "toc"])
    out = WIKI_OUT / (md_file.stem + ".html")
    out.write_text(WIKI_TMPL.format(title=title, body=body))
    print(f"wiki/{out.name}: {len(body)//1024} KB")
