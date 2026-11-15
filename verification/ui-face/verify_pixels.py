"""Assert face uniformity, 36 edge radii, centre colour, glow and mouth geometry."""
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
FACE = np.array([255, 201, 74])
# Half-open boxes cover both the original and revised speaking features,
# including antialiasing/transform margins. Identical mask for before/after.
EXCLUDED = [(140, 276, 208, 358), (276, 276, 344, 358),
            (112, 360, 178, 410), (302, 360, 368, 410),
            (186, 380, 294, 464)]


def measure(name):
    rgb = np.asarray(Image.open(HERE / f"{name}.png").convert("RGB")).astype(int)
    yy, xx = np.indices(rgb.shape[:2])
    interior = (xx-240)**2 + (yy-320)**2 <= 170**2
    for x0, y0, x1, y1 in EXCLUDED:
        interior[y0:y1, x0:x1] = False
    distance = np.max(np.abs(rgb-FACE), axis=-1)
    bad = int(np.count_nonzero(interior & (distance > 24)))
    edges = []
    # Pixel centres of a 380x380 disc span x=50..429, y=130..509.
    # Sample every 10 degrees about its geometric centre (239.5,319.5).
    for angle in range(0, 360, 10):
        theta = np.deg2rad(angle)
        radii = np.arange(175, 205, 0.05)
        sx = np.rint(239.5 + radii*np.cos(theta)).astype(int)
        sy = np.rint(319.5 + radii*np.sin(theta)).astype(int)
        inside = distance[sy, sx] <= 24
        crossings = np.where(inside[:-1] & ~inside[1:])[0]
        radius = float(radii[crossings[-1]] + 0.025) if len(crossings) else None
        edges.append(dict(degrees=angle, radius=round(radius, 3) if radius else None,
                          error=round(abs(radius-190), 3) if radius else None))
    centre = rgb[320, 240].tolist()
    centre_ok = bool(np.max(np.abs(rgb[320, 240]-FACE)) <= 8)
    radial = np.hypot(xx-239.5, yy-319.5)
    glow = []
    for r, color in [(196, [44,35,14]), (208, [30,24,9]), (220, [18,15,6])]:
        mask = np.abs(radial-r) <= 3
        count = int(np.count_nonzero(mask & (np.max(np.abs(rgb-color), axis=-1) > 8)))
        glow.append(dict(radius=r, tested=int(mask.sum()), bad_pixels=count))
    features = []
    for label, cx, cy, rx, ry, color in [
        ("left_eye",171.5,312.5,28,33,[41,32,24]),
        ("right_eye",307.5,312.5,28,33,[41,32,24]),
        ("left_cheek",144.5,377.5,29,14,[255,162,107]),
        ("right_cheek",334.5,377.5,29,14,[255,162,107]),
    ]:
        mask = ((xx-cx)/rx)**2 + ((yy-cy)/ry)**2 <= 0.36
        count = int(np.count_nonzero(mask & (np.max(np.abs(rgb-color), axis=-1) > 8)))
        features.append(dict(name=label, tested=int(mask.sum()), bad_pixels=count))
    # Ink pixels in a mouth-only ROI. A true filled ellipse occupies ~pi/4
    # of its bounding box and narrows well before the top/bottom of the box.
    ink = np.max(np.abs(rgb[380:470, 180:300]-[41,32,24]), axis=-1) <= 8
    my, mx = np.where(ink)
    mouth = None
    if len(mx):
        x0, x1, y0, y1 = int(mx.min()), int(mx.max()), int(my.min()), int(my.max())
        width, height = x1-x0+1, y1-y0+1
        top_width = int(ink[y0 + max(1, height//10)].sum())
        mouth = dict(bbox=[x0+180,y0+380,x1+180,y1+380], width=width, height=height,
                     fill_ratio=round(float(ink.sum())/(width*height), 4),
                     top_tenth_width=top_width, centre_y=round(float(my.mean()+380), 3))
        mouth["ellipse_pass"] = (1.2 <= width/height <= 2 and
                                 0.70 <= mouth["fill_ratio"] <= 0.84 and
                                 top_width < width*0.75 and mouth["centre_y"] > 410)
    edge_ok = all(p["error"] is not None and p["error"] < 2 for p in edges)
    passed = bad == 0 and edge_ok and centre_ok and all(g["bad_pixels"] == 0 for g in glow+features)
    return dict(name=name, tested_pixels=int(interior.sum()), bad_pixels=bad,
                centre=centre, centre_pass=centre_ok, edge_samples=edges, edge_pass=edge_ok,
                max_radius_error=max(p["error"] for p in edges if p["error"] is not None),
                glow=glow, feature_cores=features, mouth=mouth, face_pass=passed)


def main():
    results = [measure(n) for n in ("fb-before", "fb-after", "after") if (HERE/f"{n}.png").exists()]
    report = dict(face_color=FACE.tolist(), interior_centre=[240,320], interior_radius=170,
                  excluded_boxes=EXCLUDED, channel_tolerance=24, results=results)
    (HERE/"pixel-acceptance.json").write_text(json.dumps(report, indent=2)+"\n")
    for r in results:
        print(f'{r["name"]}: bad={r["bad_pixels"]}/{r["tested_pixels"]}, centre={r["centre"]}, '
              f'radius error max={r["max_radius_error"]}, glow={r["glow"]}, mouth={r["mouth"]}, PASS={r["face_pass"]}')
    assert any(r["name"] == "fb-after" for r in results)
    assert all(r["face_pass"] and r["mouth"] and r["mouth"]["ellipse_pass"] for r in results if r["name"] != "fb-before")
    for names, filename in [(["fb-before","fb-after"], "framebuffer-comparison.png"),
                            (["before","after"], "snapshot-comparison.png")]:
        if not all((HERE/f"{n}.png").exists() for n in names):
            continue
        sheet = Image.new("RGB", (960, 884), "#202020")
        draw = ImageDraw.Draw(sheet)
        for i, name in enumerate(names):
            draw.text((i*480+12, 8), name, fill="white")
            sheet.paste(Image.open(HERE/f"{name}.png"), (i*480, 30))
        sheet.save(HERE/filename)


if __name__ == "__main__":
    main()
