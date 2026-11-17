"""Packs frames (sorted by name) into a row-major padded atlas for prepareFlipbookCard.
usage: python atlas.py <frames_dir> <out.png> [pad=2]   -> prints the metadata JSON
"""
import json, math, sys
from pathlib import Path
from PIL import Image
src, out, pad = Path(sys.argv[1]), sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 2
files = sorted(p for p in src.glob("*.png"))
imgs = [Image.open(p).convert("RGBA") for p in files]
w, h = imgs[0].size
cols = math.ceil(math.sqrt(len(imgs))); rows = math.ceil(len(imgs) / cols)
cw, ch = w + 2 * pad, h + 2 * pad
atlas = Image.new("RGBA", (cols * cw, rows * ch), (0, 0, 0, 0))
for i, im in enumerate(imgs):
    x, y = (i % cols) * cw, (i // cols) * ch
    # surround the original frame with its expanded image to reduce mip bleeding
    atlas.paste(im.resize((cw, ch)), (x, y)); atlas.paste(im, (x + pad, y + pad))
atlas.save(out)
print(json.dumps({"columns": cols, "rows": rows, "frameCount": len(imgs), "framesPerSecond": 24, "loop": True, "playing": True}))
