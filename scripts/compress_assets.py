# compress_assets.py
# PlatformIO pre-script: gzip kompresija src/assets/ → data/assets/
# Kliče se pred vsakim buildom (extra_scripts = pre:scripts/compress_assets.py)
#
# SPREMEMBE:
#   v1.1 (2026-05-14): Dodan cleanup zastarelih datotek iz data/assets/
#   v1.2 (2026-05-14): index.html dodan v NO_GZIP — ostane nekompresiran.
#     Razlog: web_ui.cpp kliče serveStatic().setDefaultFile("index.html") in
#     LittleFS.exists("/assets/index.html") — oba zahtevata nekompresiran HTML.
#     ESPAsyncWebServer NE išče index.html.gz samodejno za default file.

import os, gzip, shutil
Import("env")

SRC_DIR  = os.path.join(env.subst("$PROJECT_DIR"), "src", "assets")
DATA_DIR = os.path.join(env.subst("$PROJECT_DIR"), "data", "assets")

# .html je v NO_GZIP ker web_ui.cpp išče index.html direktno (setDefaultFile).
# Fonti (.woff2) so binarni in jih gzip ne stisne učinkovito.
NO_GZIP  = {".gz", ".jpg", ".jpeg", ".png", ".ico", ".woff", ".woff2", ".html"}

def compress_assets(*args, **kwargs):
    if not os.path.isdir(SRC_DIR):
        print("[compress_assets] src/assets/ ne obstaja — preskoči")
        return
    os.makedirs(DATA_DIR, exist_ok=True)

    # ── Izgradi množico pričakovanih izhodnih datotek ────────────────────
    # Za vsako datoteko v src/ vemo točno kaj bo nastalo v data/:
    #   - NO_GZIP → fname (kopija)
    #   - ostalo  → fname + ".gz"
    expected = set()
    for fname in os.listdir(SRC_DIR):
        src_path = os.path.join(SRC_DIR, fname)
        if not os.path.isfile(src_path) or fname.startswith('.'):
            continue
        ext = os.path.splitext(fname)[1].lower()
        if ext in NO_GZIP:
            expected.add(fname)
        else:
            expected.add(fname + ".gz")

    # ── Cleanup: zbriši iz data/assets/ vse kar ni v expected ────────────
    # Izjema: skrite datoteke (.gitkeep ipd.)
    # Primer tega ki bo zbrisano: index.html.gz (ker .html je zdaj v NO_GZIP
    # in expected vsebuje samo "index.html", ne "index.html.gz")
    removed = 0
    for fname in os.listdir(DATA_DIR):
        if fname.startswith('.'):
            continue
        if fname not in expected:
            stale = os.path.join(DATA_DIR, fname)
            if os.path.isfile(stale):
                os.remove(stale)
                print(f"[compress_assets] CLEANUP: odstranil zastarelo datoteko: {fname}")
                removed += 1
    if removed:
        print(f"[compress_assets] cleanup: odstranjenih {removed} zastarelih datotek")

    # ── Kompresija / kopiranje ────────────────────────────────────────────
    count = 0
    for fname in os.listdir(SRC_DIR):
        src_path = os.path.join(SRC_DIR, fname)
        if not os.path.isfile(src_path) or fname.startswith('.'):
            continue
        ext = os.path.splitext(fname)[1].lower()
        if ext in NO_GZIP:
            shutil.copy2(src_path, os.path.join(DATA_DIR, fname))
            print(f"[compress_assets] kopiran (brez gzip): {fname}")
        else:
            dst = os.path.join(DATA_DIR, fname + ".gz")
            with open(src_path, "rb") as fi:
                with gzip.open(dst, "wb", compresslevel=9) as fo:
                    shutil.copyfileobj(fi, fo)
            src_kb = os.path.getsize(src_path) // 1024
            dst_kb = os.path.getsize(dst) // 1024
            print(f"[compress_assets] {fname} → {fname}.gz  ({src_kb} KB → {dst_kb} KB)")
            count += 1
    print(f"[compress_assets] skupaj {count} datotek gzip + index.html nekompresiran v data/assets/")

env.AddPreAction("buildprog", compress_assets)
env.AddPreAction("uploadfs",  compress_assets)
