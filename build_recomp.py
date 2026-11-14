import os
import sys
import subprocess
import time

PYTHON = r"C:\Users\SMANSA\hermes-webui\.venv\Scripts\python.exe"
NDK_CLANG = r"D:\switchRecomp\android-sdk\ndk\30.0.16248370\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android29-clang.cmd"
REPO_DIR = r"D:\switchRecomp\RyzaNativeAndroid"
OUT_C_DIR = os.path.join(REPO_DIR, "recomp_c")
OUT_LIB = os.path.join(REPO_DIR, "botwgame.so")

MODULES = [
    ("main", os.path.join(REPO_DIR, "game", "exefs", "main"), "0x7000000000"),
    ("subsdk0", os.path.join(REPO_DIR, "game", "exefs", "subsdk0"), "0x7002000000"),
    ("sdk", os.path.join(REPO_DIR, "game", "exefs", "sdk"), "0x7004000000"),
]

def run(cmd, cwd=None):
    print(f"[RUN] {cmd}")
    res = subprocess.run(cmd, shell=True, cwd=cwd)
    if res.returncode != 0:
        print(f"[ERROR] Command failed with code {res.returncode}")
        sys.exit(res.returncode)

def main():
    os.makedirs(OUT_C_DIR, exist_ok=True)
    hle_txt = os.path.join(REPO_DIR, "runtime", "hle", "hle_symbols.txt")

    print("=== TAHAP 1: Generating C source files via tools/gen.py ===")
    for name, nso_path, base in MODULES:
        print(f"\n--- Generating {name} (base {base}) ---")
        outdir = os.path.join(OUT_C_DIR, name)
        os.makedirs(outdir, exist_ok=True)
        cmd = f'"{PYTHON}" tools/gen.py {name} "{nso_path}" {base} "{outdir}" --hle "{hle_txt}" --dyn-base --simd c'
        run(cmd, cwd=REPO_DIR)

    print("\n=== TAHAP 2: Compiling C files to botwgame.so (Android ARM64) ===")
    c_files = []
    for root, dirs, files in os.walk(OUT_C_DIR):
        for f in files:
            if f.endswith(".c"):
                c_files.append(os.path.join(root, f))

    entry_c = os.path.join(REPO_DIR, "tools", "gamelib_entry.c")
    c_files.append(entry_c)

    print(f"Total C files to compile: {len(c_files)}")

    # Buat response file untuk menangani ratusan file C di command line Windows
    rsp_file = os.path.join(OUT_C_DIR, "compile.rsp")
    with open(rsp_file, "w") as f:
        for cf in c_files:
            f.write(f'"{cf.replace(chr(92), "/")}"\n')

    include_dir = os.path.join(REPO_DIR, "runtime")
    compile_cmd = (
        f'"{NDK_CLANG}" -O2 -fPIC -shared -DHAS_SUBSDK0 '
        f'-I"{include_dir}" '
        f'@{rsp_file} -o "{OUT_LIB}"'
    )
    run(compile_cmd, cwd=REPO_DIR)

    print(f"\n[SUCCESS] botwgame.so berhasil dibuat di: {OUT_LIB}")
    print(f"File size: {os.path.getsize(OUT_LIB)} bytes")

if __name__ == "__main__":
    main()
