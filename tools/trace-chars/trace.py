#!/usr/bin/env python3
"""trace.py — extract simplified polyline contours from a calligraphy image.

Reads:  the reference image (SRC below)
Writes: data/sketches/examples/wo_ai_ni.js   (the on-device sketch)
        tools/trace-chars/debug/*.png        (binary mask + per-char overlays)

The image is binarized, split into N characters by column projection, then
each character's contours (outer outline + interior holes) are extracted with
OpenCV and Douglas-Peucker-simplified to a manageable number of points. The
generated sketch then "inks" each contour segment-by-segment on the LCD.

Tweak THRESHOLD / EPSILON_PCT / MIN_AREA_PCT to trade fidelity for point count.
Run:  python tools/trace-chars/trace.py
"""

import json
import os
import re
import sys
import cv2
import numpy as np
from PIL import Image

# ── inputs / outputs ────────────────────────────────────────────────────
REPO        = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SRC         = r"C:\Users\Harvey\Pictures\Screenshots\Screenshot 2026-05-18 014023.png"
SKETCH_OUT  = os.path.join(REPO, "data", "sketches", "examples", "wo_ai_ni.js")
DOCS_INDEX  = os.path.join(REPO, "docs", "index.html")
DEBUG_DIR   = os.path.join(os.path.dirname(__file__), "debug")
SKETCH_NAME = "wo_ai_ni"   # key under which the sketch appears in the companion dropdown

# ── tuning ──────────────────────────────────────────────────────────────
THRESHOLD     = 100      # pixel < this → ink (filters faint watermark)
EPSILON_PCT   = 0.0025   # approxPolyDP epsilon as a fraction of perimeter
MIN_AREA_PCT  = 0.005    # drop contours smaller than this fraction of char bbox
COL_GAP_PX    = 18       # merge runs within this many empty columns

CHAR_NAMES = ["wo", "ai", "ni"]


def find_char_columns(bin_img):
    """Return up to N column ranges (x0, x1), one per character, left-to-right."""
    H, W = bin_img.shape
    col_sum = bin_img.sum(axis=0)

    runs = []
    in_run = False
    start = 0
    for x in range(W):
        if col_sum[x] > 0 and not in_run:
            in_run = True
            start = x
        elif col_sum[x] == 0 and in_run:
            in_run = False
            runs.append((start, x))
    if in_run:
        runs.append((start, W))

    merged = []
    for s, e in runs:
        if merged and s - merged[-1][1] < COL_GAP_PX:
            merged[-1] = (merged[-1][0], e)
        else:
            merged.append((s, e))

    merged.sort(key=lambda r: r[1] - r[0], reverse=True)
    top = sorted(merged[: len(CHAR_NAMES)], key=lambda r: r[0])
    return top


def extract_contours(crop):
    """Return list of (contour_pts_Nx2, area) sorted by area desc."""
    contours, hierarchy = cv2.findContours(
        crop, cv2.RETR_CCOMP, cv2.CHAIN_APPROX_SIMPLE
    )
    crop_area = crop.shape[0] * crop.shape[1]
    out = []
    for c in contours:
        a = cv2.contourArea(c)
        if a < crop_area * MIN_AREA_PCT:
            continue
        perim = cv2.arcLength(c, True)
        eps = max(1.0, EPSILON_PCT * perim)
        simplified = cv2.approxPolyDP(c, eps, True)
        out.append((simplified, a))
    out.sort(key=lambda t: t[1], reverse=True)
    return out


def normalize(pts, cw, ch):
    """Map crop pixel coords into a 0..100 box with uniform scale + centering."""
    scale = 100.0 / max(cw, ch)
    ox = (100.0 - cw * scale) / 2.0
    oy = (100.0 - ch * scale) / 2.0
    return [(ox + x * scale, oy + y * scale) for x, y in pts]


def render_overlay(crop, contours, path):
    rgb = cv2.cvtColor(crop, cv2.COLOR_GRAY2BGR)
    for c, _a in contours:
        cv2.drawContours(rgb, [c], -1, (0, 200, 0), 1)
        for pt in c.reshape(-1, 2):
            cv2.circle(rgb, tuple(int(v) for v in pt), 2, (0, 0, 255), -1)
    Image.fromarray(cv2.cvtColor(rgb, cv2.COLOR_BGR2RGB)).save(path)


def process():
    os.makedirs(DEBUG_DIR, exist_ok=True)

    if not os.path.exists(SRC):
        print(f"ERROR: source image not found: {SRC}", file=sys.stderr)
        sys.exit(1)

    im = Image.open(SRC).convert("L")
    arr = np.array(im)
    bin_img = (arr < THRESHOLD).astype(np.uint8) * 255
    Image.fromarray(bin_img).save(os.path.join(DEBUG_DIR, "binary.png"))
    print(f"Source: {SRC}  ({im.size[0]}x{im.size[1]})")
    print(f"Threshold: {THRESHOLD}  ->  binary.png written")

    ranges = find_char_columns(bin_img)
    if len(ranges) < len(CHAR_NAMES):
        print(
            f"WARNING: found {len(ranges)} column runs, expected {len(CHAR_NAMES)} — "
            "falling back to equal slices."
        )
        third = bin_img.shape[1] // len(CHAR_NAMES)
        ranges = [
            (i * third, (i + 1) * third if i + 1 < len(CHAR_NAMES) else bin_img.shape[1])
            for i in range(len(CHAR_NAMES))
        ]
    print(f"Character columns: {ranges}")

    chars_data = []
    for i, (cx0, cx1) in enumerate(ranges):
        name = CHAR_NAMES[i]
        slab = bin_img[:, cx0:cx1]
        ys, xs = np.where(slab > 0)
        if len(xs) == 0:
            print(f"  {name}: empty slab, skipping")
            continue
        by0, by1 = ys.min(), ys.max() + 1
        bx0, bx1 = xs.min(), xs.max() + 1
        crop = slab[by0:by1, bx0:bx1]
        ch, cw = crop.shape

        contours = extract_contours(crop)
        polylines = [normalize(c.reshape(-1, 2), cw, ch) for c, _ in contours]

        render_overlay(crop, contours, os.path.join(DEBUG_DIR, f"{name}_overlay.png"))

        total = sum(len(p) for p in polylines)
        print(f"  {name}: {len(polylines)} contours, {total} pts  (crop {cw}x{ch})")
        chars_data.append((name, polylines))

    write_sketch(chars_data)


SKETCH_TEMPLATE = """\
// wo_ai_ni.js — 我 爱 你
//
// AUTO-GENERATED by tools/trace-chars/trace.py from a brush-calligraphy
// reference. Each character is a list of CLOSED polylines (outer outline
// + interior holes), traced via OpenCV and Douglas-Peucker-simplified.
//
// The sketch inks each contour segment-by-segment so the character draws
// itself, holds, clears, then advances to the next character. Per-frame
// work is essentially one _ln() call — extremely light on the interpreter.

{data_block}
let chars = [{chars_list}];

let CX = 120;
let CY = 120;
let SCALE = 2.0;        // 0..100 local box → 0..200 screen
let HALF = 50;

let charIdx = 0;
let stage = 0;          // 0=ink-outline, 1=hold-line, 2=fade-fill, 3=hold-fill
let polyIdx = 0;
let segIdx  = 0;
let progress = 0;       // 0..1 along current segment
let holdFrames = 0;
let fillFrame  = 0;

let SPEED        = 0.525;   // outline segment fraction per frame
let HOLD_LINE    = 30;      // frames to admire the colored outline (~1 s)
let FILL_FRAMES  = 18;      // frames of the fade-to-white animation (~0.6 s)
let HOLD_FILL    = 45;      // frames to admire the filled white character (~1.5 s)
let FILL_R = 240;
let FILL_G = 240;
let FILL_B = 240;

function setup() {{
  autoRotate();
  _bg(8, 10, 18);
}}

function draw() {{
  let cur = chars[charIdx];

  // ── stage 0: ink the colored outline, one chunk per frame ──
  if (stage ===0) {{
    if (polyIdx >= cur.length) {{
      stage = 1;
      holdFrames = 0;
      return;
    }}
    let p = cur[polyIdx];
    let nPts = p.length / 2;
    if (segIdx >= nPts) {{
      polyIdx = polyIdx + 1;
      segIdx = 0;
      progress = 0;
      return;
    }}
    let aIdx = segIdx * 2;
    let bIdx = segIdx + 1;
    if (bIdx >= nPts) {{ bIdx = 0; }}
    let bOff = bIdx * 2;
    let x0 = CX + (p[aIdx]     - HALF) * SCALE;
    let y0 = CY + (p[aIdx + 1] - HALF) * SCALE;
    let x1 = CX + (p[bOff]     - HALF) * SCALE;
    let y1 = CY + (p[bOff + 1] - HALF) * SCALE;
    let newProg = progress + SPEED;
    if (newProg > 1) {{ newProg = 1; }}
    let dx = x1 - x0;
    let dy = y1 - y0;
    let px0 = x0 + dx * progress;
    let py0 = y0 + dy * progress;
    let px1 = x0 + dx * newProg;
    let py1 = y0 + dy * newProg;
    _ska(themeR, themeG, themeB, 240);
    _sw(2);
    _ln(px0, py0, px1, py1);
    progress = newProg;
    if (progress >= 1) {{
      segIdx = segIdx + 1;
      progress = 0;
    }}
    return;
  }}

  // ── stage 1: hold the colored outline ──
  if (stage ===1) {{
    holdFrames = holdFrames + 1;
    if (holdFrames > HOLD_LINE) {{
      stage = 2;
      fillFrame = 0;
    }}
    return;
  }}

  // ── stage 2: fade-fill — redraw all outlines with color → white,
  //    weight 2 → 8 over FILL_FRAMES frames. The fat white stroke
  //    covers the narrow brush shapes, "filling" them. ──
  if (stage ===2) {{
    let t = fillFrame / FILL_FRAMES;
    if (t > 1) {{ t = 1; }}
    let r = (themeR + (FILL_R - themeR) * t) | 0;
    let g = (themeG + (FILL_G - themeG) * t) | 0;
    let b = (themeB + (FILL_B - themeB) * t) | 0;
    let w = (2 + 6 * t) | 0;
    _ska(r, g, b, 240);
    _sw(w);
    let pi;
    for (pi = 0; pi < cur.length; pi = pi + 1) {{
      let p = cur[pi];
      let nPts = p.length / 2;
      let si;
      for (si = 0; si < nPts; si = si + 1) {{
        let aIdx = si * 2;
        let bIdx = si + 1;
        if (bIdx >= nPts) {{ bIdx = 0; }}
        let bOff = bIdx * 2;
        let x0 = CX + (p[aIdx]     - HALF) * SCALE;
        let y0 = CY + (p[aIdx + 1] - HALF) * SCALE;
        let x1 = CX + (p[bOff]     - HALF) * SCALE;
        let y1 = CY + (p[bOff + 1] - HALF) * SCALE;
        _ln(x0, y0, x1, y1);
      }}
    }}
    fillFrame = fillFrame + 1;
    if (fillFrame > FILL_FRAMES) {{
      stage = 3;
      holdFrames = 0;
    }}
    return;
  }}

  // ── stage 3: hold the filled white character, then clear + advance ──
  if (stage ===3) {{
    holdFrames = holdFrames + 1;
    if (holdFrames > HOLD_FILL) {{
      _bg(8, 10, 18);
      charIdx = charIdx + 1;
      if (charIdx >= chars.length) {{ charIdx = 0; }}
      stage = 0;
      polyIdx = 0;
      segIdx = 0;
      progress = 0;
      holdFrames = 0;
      fillFrame = 0;
    }}
    return;
  }}
}}
"""


def write_sketch(chars_data):
    data_lines = []
    chars_list = []
    for name, polylines in chars_data:
        var = f"{name}_outlines"
        chars_list.append(var)
        data_lines.append(f"let {var} = [")
        for poly in polylines:
            flat = ", ".join(f"{x:.1f}, {y:.1f}" for x, y in poly)
            data_lines.append(f"  [{flat}],")
        data_lines.append("];")
        data_lines.append("")
    data_block = "\n".join(data_lines)

    out = SKETCH_TEMPLATE.format(
        data_block=data_block,
        chars_list=", ".join(chars_list),
    )
    with open(SKETCH_OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(out)
    print(f"Wrote {SKETCH_OUT}  ({len(out)} bytes)")

    update_companion_examples(out)


def update_companion_examples(sketch_text):
    """Add/update SKETCH_NAME in the companion's examples-data JSON block."""
    if not os.path.exists(DOCS_INDEX):
        print(f"WARN: {DOCS_INDEX} not found, skipping companion update")
        return

    with open(DOCS_INDEX, "r", encoding="utf-8") as f:
        html = f.read()

    pattern = re.compile(
        r'(<script id="examples-data" type="application/json">)(.*?)(</script>)',
        re.DOTALL,
    )
    m = pattern.search(html)
    if not m:
        print(f"ERROR: <script id='examples-data'> block not found in {DOCS_INDEX}")
        return

    raw = m.group(2).strip()
    try:
        examples = json.loads(raw)
    except json.JSONDecodeError as e:
        print(f"ERROR: failed to parse examples-data JSON: {e}")
        return

    existed = SKETCH_NAME in examples
    examples[SKETCH_NAME] = sketch_text

    # Re-serialize one entry per line so the diff stays small and readable.
    items = [
        f"  {json.dumps(k, ensure_ascii=False)}: {json.dumps(v, ensure_ascii=False)}"
        for k, v in examples.items()
    ]
    new_block = "\n" + "{\n" + ",\n".join(items) + "\n}\n"
    new_html = html[: m.start(2)] + new_block + html[m.end(2):]

    if new_html == html:
        return  # no change

    with open(DOCS_INDEX, "w", encoding="utf-8", newline="\n") as f:
        f.write(new_html)
    verb = "updated" if existed else "added"
    print(f"Companion examples: {verb} '{SKETCH_NAME}' in {DOCS_INDEX}")


if __name__ == "__main__":
    process()
