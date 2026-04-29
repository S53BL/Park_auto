# compress_assets.py
# PlatformIO pre-script: gzip kompresija src/assets/ → data/assets/
# Kliče se pred vsakim buildom (extra_scripts = pre:scripts/compress_assets.py)
import os, gzip, shutil
Import("env")

SRC_DIR  = os.path.join(env.subst("$PROJECT_DIR"), "src", "assets")
DATA_DIR = os.path.join(env.subst("$PROJECT_DIR"), "data", "assets")
NO_GZIP  = {".gz", ".jpg", ".jpeg", ".png", ".ico", ".woff", ".woff2"}

def compress_assets(*args, **kwargs):
    if not os.path.isdir(SRC_DIR):
        print("[compress_assets] src/assets/ ne obstaja — preskoči")
        return
    os.makedirs(DATA_DIR, exist_ok=True)
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
    print(f"[compress_assets] skupaj {count} datotek gzip kompresiranih v data/assets/")

env.AddPreAction("buildprog", compress_assets)
env.AddPreAction("uploadfs",  compress_assets)
