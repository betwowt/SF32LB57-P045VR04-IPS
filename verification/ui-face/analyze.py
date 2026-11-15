"""Measure unmodified, on-device LVGL PNGs and build a comparison sheet."""
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
# Radius 234 from face centre: outside both the 190 px face and 226 px glow.
OUTSIDE = [(240, 86), (405, 154), (474, 320), (405, 485),
           (240, 554), (74, 485), (6, 320), (74, 154)]


def measure(name, opened):
    im = np.asarray(Image.open(HERE / f"{name}.png").convert("RGB"))
    # The ink is RGB565 #292018. Limit to the mouth so eyes/glow cannot count.
    roi = im[360:480, 180:300]
    ink = np.max(np.abs(roi.astype(int) - (41, 32, 24)), axis=2) <= 8
    yy, xx = np.where(ink)
    assert len(xx), f"{name}: mouth missing"
    x0, y0, x1, y1 = int(xx.min()+180), int(yy.min()+360), int(xx.max()+180), int(yy.max()+360)
    width, height = x1-x0+1, y1-y0+1
    corners = [im[y, x].tolist() for x, y in [(x0,y0),(x1,y0),(x0,y1),(x1,y1)]]
    corner_clear = all(max(abs(c-n) for c,n in zip(p, (41,32,24))) > 8 for p in corners)
    # Both cheeks, excluding the mouth; derive the cheek centroid from pixels.
    cheeks = im[350:415, 100:380]
    mask = (cheeks[:,:,0] > 245) & (cheeks[:,:,1] >= 130) & (cheeks[:,:,1] <= 180) & (cheeks[:,:,2] >= 80) & (cheeks[:,:,2] <= 125)
    cy, _ = np.where(mask)
    cheek_y = float(cy.mean()+350)
    mouth_y = float(yy.mean()+360)
    outside = [im[y,x].tolist() for x,y in OUTSIDE]
    result = dict(name=name, open=opened, bbox=[x0,y0,x1,y1], width=width, height=height,
                  ratio=round(width/height,4), corners=corners, corners_clear=corner_clear,
                  mouth_centroid_y=round(mouth_y,3), cheek_centroid_y=round(cheek_y,3),
                  centroid_below_cheeks=mouth_y>=cheek_y, outside_samples=outside,
                  outside_black=all(p==[0,0,0] for p in outside),
                  bbox_fill=round(int(ink.sum())/(width*height),4),
                  face_centre=im[320,240].tolist())
    result["pass"] = result["centroid_below_cheeks"] and result["outside_black"] and (not opened or (1.2<=width/height<=2.0 and corner_clear))
    return result


def sheet(names, output, crop=False):
    w,h = (280,220) if crop else (480,854)
    cols = min(4,len(names))
    canvas = Image.new("RGB",(w*cols,(h+36)*((len(names)+cols-1)//cols)), "#202020")
    draw = ImageDraw.Draw(canvas)
    for i,name in enumerate(names):
        im = Image.open(HERE/f"{name}.png").convert("RGB")
        if crop: im = im.crop((100,260,380,480))
        x,y=(i%cols)*w,(i//cols)*(h+36)
        draw.text((x+10,y+10),name,fill="white")
        canvas.paste(im,(x,y+36))
    canvas.save(HERE/output)


def main():
    states = {"before":True, "after":True, "speak":True, "listen":True,
              "idle":False, "smile":False, "frown":False, "think":True,
              "connect":False, "error":False, "sleep":False, "config":True,
              **{f"speak{i}":True for i in range(6)}}
    results=[measure(n,o) for n,o in states.items() if (HERE/f"{n}.png").exists()]
    (HERE/"measurements.json").write_text(json.dumps({"ink_rgb":[41,32,24],"ink_tolerance":8,"outside_points":OUTSIDE,"results":results},indent=2)+"\n")
    if (HERE/"after.png").exists(): sheet(["before","after"],"before-after.png")
    names=[r["name"] for r in results if r["name"] not in ("before","after")]
    if names: sheet(names,"states.png",crop=True)
    for r in results:
        print(f'{r["name"]:8s} {r["width"]}x{r["height"]} ratio={r["ratio"]} centroid={r["mouth_centroid_y"]}/{r["cheek_centroid_y"]} corners={r["corners_clear"]} outside={r["outside_black"]} PASS={r["pass"]}')
    assert all(r["pass"] for r in results if r["name"]!="before")


if __name__ == "__main__":
    main()
