#!/usr/bin/env python3
"""Live test dashboard for Termux:X11.

Every second, renders /tmp/live/dashboard.png (portrait, 1080x2055) from:
  - the CTS runner status file  (runs/live_status.json): group, current case
  - the group's results.tsv     : counts and the most recent results
  - the newest image found in the live image folder or in the current .qpa
    log (CTS embeds result/reference images as base64 PNG)
feh shows the PNG fullscreen and reloads it, so the screen follows the run.
"""
import base64, collections, glob, io, json, os, re, time
from PIL import Image, ImageDraw, ImageFont

C = "/data/data/com.termux/files/home/cts"
LIVE_IMG = "/data/data/com.termux/files/home/panvk-g57/phase4/img/live"
OUT_DIR = "/tmp/live"
OUT = OUT_DIR + "/dashboard.png"
W, H = 1080, 2055
COL = {"Pass": (80, 200, 120), "Fail": (235, 80, 80), "NotSupported": (150, 150, 150),
       "Crash": (255, 60, 200), "Timeout": (255, 170, 40), "QualityWarning": (230, 210, 70),
       "CompatibilityWarning": (230, 210, 70), "InternalError": (255, 60, 200)}


def font(sz):
    for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        if os.path.exists(p):
            return ImageFont.truetype(p, sz)
    return ImageFont.load_default()


F_BIG, F_MID, F_SM = font(54), font(34), font(26)
img_re = re.compile(r'<Image Name="([^"]*)"[^>]*Format="PNG"[^>]*>\s*([A-Za-z0-9+/=\s]+?)\s*</Image>')


def latest_qpa_image(qpa):
    try:
        with open(qpa, "rb") as f:
            f.seek(max(0, os.path.getsize(qpa) - 4 * 1024 * 1024))
            tail = f.read().decode("utf-8", "ignore")
    except OSError:
        return None, None
    found = img_re.findall(tail)
    for name, b64 in reversed(found):
        try:
            return name, Image.open(io.BytesIO(base64.b64decode("".join(b64.split())))).convert("RGB")
        except Exception:
            continue
    return None, None


def latest_live_png():
    files = glob.glob(LIVE_IMG + "/*.png")
    if not files:
        return None, None, 0
    f = max(files, key=os.path.getmtime)
    try:
        return os.path.basename(f), Image.open(f).convert("RGB"), os.path.getmtime(f)
    except Exception:
        return None, None, 0


def render():
    st = {}
    try:
        st = json.load(open(C + "/runs/live_status.json"))
    except Exception:
        pass
    group = st.get("group", "-")
    cur = st.get("current", "")
    total = st.get("total", 0)
    res = []
    rp = f"{C}/runs/{group}/results.tsv"
    if os.path.exists(rp):
        res = [l.rstrip("\n").split("\t") for l in open(rp) if "\t" in l]
    cnt = collections.Counter(r[1] for r in res)

    im = Image.new("RGB", (W, H), (18, 20, 26))
    d = ImageDraw.Draw(im)
    d.text((40, 30), "PanVK  Mali-G57 MC2", font=F_BIG, fill=(240, 240, 240))
    d.text((40, 100), time.strftime("%H:%M:%S") + ("   RUNNING" if st.get("running") else "   idle"),
           font=F_MID, fill=(120, 200, 255) if st.get("running") else (140, 140, 140))
    d.text((40, 160), f"group: {group}", font=F_MID, fill=(220, 220, 220))
    done = len(res)
    d.text((40, 210), f"{done} / {total} case", font=F_MID, fill=(220, 220, 220))
    # progress bar
    d.rectangle((40, 265, W - 40, 305), outline=(90, 90, 90), width=2)
    if total:
        x = 40
        for k in ("Pass", "Fail", "Crash", "Timeout", "NotSupported", "QualityWarning", "InternalError"):
            n = cnt.get(k, 0)
            if n:
                w = (W - 84) * n / total
                d.rectangle((x + 2, 267, x + 2 + w, 303), fill=COL[k])
                x += w
    y = 325
    for k in ("Pass", "Fail", "Crash", "Timeout", "NotSupported", "QualityWarning", "InternalError"):
        if cnt.get(k):
            d.text((40, y), f"{k:14s} {cnt[k]}", font=F_MID, fill=COL[k]); y += 44
    d.text((40, y + 10), "sekarang:", font=F_SM, fill=(150, 150, 150))
    d.text((40, y + 45), cur.replace("dEQP-VK.", "")[-60:], font=F_SM, fill=(255, 255, 255))

    # image panel: newest of live folder PNG and CTS qpa image
    name, pic, src = None, None, ""
    ln, li, lt = latest_live_png()
    qpa = st.get("qpa")
    qn, qi = latest_qpa_image(qpa) if qpa else (None, None)
    if qi is not None and (li is None or (qpa and os.path.getmtime(qpa) > lt)):
        name, pic, src = qn, qi, "CTS"
    elif li is not None:
        name, pic, src = ln, li, "test lokal"
    top = y + 110
    if pic is not None:
        side = min(W - 80, H - top - 520)
        scale = max(1, side // max(pic.width, pic.height))
        big = pic.resize((pic.width * scale, pic.height * scale), Image.NEAREST)
        im.paste(big, ((W - big.width) // 2, top))
        d.text((40, top + big.height + 10), f"gambar GPU ({src}): {name}  {pic.width}x{pic.height}",
               font=F_SM, fill=(170, 170, 170))
        top += big.height + 60
    # last results
    d.text((40, top), "hasil terakhir:", font=F_SM, fill=(150, 150, 150)); top += 36
    for r in res[-12:][::-1]:
        if top > H - 40:
            break
        d.text((40, top), f"{r[1][:12]:12s} " + r[0].replace("dEQP-VK.", "")[-52:], font=F_SM,
               fill=COL.get(r[1], (200, 200, 200)))
        top += 34
    os.makedirs(OUT_DIR, exist_ok=True)
    tmp = OUT + ".tmp.png"
    im.save(tmp)
    os.replace(tmp, OUT)


if __name__ == "__main__":
    while True:
        try:
            render()
        except Exception as e:
            print("dashboard:", e, flush=True)
        time.sleep(1)
