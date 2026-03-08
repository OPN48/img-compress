from pathlib import Path
import os
import json
import shutil
import subprocess


def run_command(command: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
    try:
        p = subprocess.Popen(
            command,
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            universal_newlines=True,
            encoding="utf-8",
            errors="replace",
            env=env,
        )
    except PermissionError:
        exe = str(command[0])
        cmdline = " ".join(f'"{c}"' if (" " in c and not c.startswith('"')) else c for c in command)
        try:
            p = subprocess.Popen(
                cmdline,
                cwd=str(cwd),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                universal_newlines=True,
                encoding="utf-8",
                errors="replace",
                env=env,
                shell=True,
            )
        except Exception as e:
            raise SystemExit(f"无法执行命令（权限问题或路径不可访问）：{exe}\n请确保可执行文件存在且可运行，或在 VS 开发者命令提示符下重试。\n详细错误：{e}")
    s = p.stdout
    if s is not None:
        for line in s:
            print(line, end="")
    p.wait()
    if p.returncode != 0:
        raise SystemExit(p.returncode)


def load_config(root: Path) -> dict:
    cfg = {}
    ext = os.environ.get("IMG_COMPRESS_CONFIG")
    if ext and Path(ext).exists():
        try:
            cfg = json.loads(Path(ext).read_text(encoding="utf-8"))
        except Exception:
            cfg = {}
    else:
        f = root / "build_config.windows.json"
        if f.exists():
            try:
                cfg = json.loads(f.read_text(encoding="utf-8"))
            except Exception:
                cfg = {}
    return cfg


def load_app_config(root: Path) -> dict:
    cfg_path = root / "app_config.json"
    if cfg_path.exists():
        try:
            return json.loads(cfg_path.read_text(encoding="utf-8"))
        except Exception:
            return {}
    return {}


def read_app_value(cfg: dict, key: str, default: str) -> str:
    v = cfg.get(key)
    return str(v) if v else default


def cfg_get(cfg: dict, name: str) -> str | None:
    v = os.environ.get(name)
    if v:
        return v
    x = cfg.get(name)
    if x is None:
        return None
    if isinstance(x, list):
        return ";".join(str(i) for i in x if i)
    return str(x)


def cfg_list(cfg: dict, name: str) -> list[str]:
    v = cfg_get(cfg, name)
    if not v:
        return []
    return [i for i in v.split(";") if i]


def resolve_tool(default_name: str, cfg_keys: list[str], name_candidates: list[str]) -> str:
    for k in cfg_keys:
        v = cfg_get(cfg_global, k)
        if not v:
            continue
        p = Path(v)
        if p.is_dir():
            for n in name_candidates:
                cand = p / n
                if cand.exists():
                    return str(cand)
        elif p.exists():
            return str(p)
    w = shutil.which(default_name)
    if w:
        return w
    raise SystemExit(f"未找到 {default_name}，请在配置或环境中指定")


def resolve_generator_args(cmake: str, cfg: dict) -> list[str]:
    g = cfg_get(cfg, "CMAKE_GENERATOR")
    if g:
        if g.lower() == "ninja":
            mp = cfg_get(cfg, "CMAKE_MAKE_PROGRAM") or resolve_tool("ninja", ["NINJA_EXE", "NINJA"], ["ninja.exe", "ninja"])
            return ["-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + mp]
        if g.lower().startswith("visual studio"):
            return ["-G", g, "-A", "x64"]
        return ["-G", g]
    mp = shutil.which("ninja")
    if mp:
        return ["-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + mp]
    if shutil.which("nmake"):
        return ["-G", "NMake Makefiles"]
    raise SystemExit("未找到可用生成器，请设置 CMAKE_GENERATOR 或安装 Ninja/NMake")


def read_cache_generator(build_dir: Path) -> str | None:
    f = build_dir / "CMakeCache.txt"
    if not f.exists():
        return None
    try:
        for line in f.read_text(encoding="utf-8", errors="ignore").splitlines():
            line = line.strip()
            if line.startswith("CMAKE_GENERATOR:") and "=" in line:
                return line.split("=", 1)[1]
    except Exception:
        return None
    return None


def ensure_clean_build_dir(build_dir: Path, expected_generator: str | None) -> None:
    if not expected_generator:
        return
    cached = read_cache_generator(build_dir)
    if cached and cached.strip() != expected_generator.strip():
        if build_dir.exists():
            shutil.rmtree(build_dir)


def resolve_qt_prefix(cfg: dict) -> str | None:
    v = cfg_get(cfg, "QT_PREFIX") or cfg_get(cfg, "CMAKE_PREFIX_PATH")
    if not v:
        return None
    return str(Path(v)).replace("\\", "/")


def resolve_exe(build_dir: Path, exe_name: str) -> Path:
    target = f"{exe_name}.exe"
    for p in [build_dir / target, build_dir / "Release" / target]:
        if p.exists():
            return p
    raise SystemExit(f"未找到 {target}")


def compose_env(cfg: dict) -> dict[str, str]:
    env = dict(os.environ)
    mb = cfg_list(cfg, "MSVC_BIN")
    sb = cfg_list(cfg, "WINSDK_BIN")
    if mb:
        env["PATH"] = ";".join(mb) + ";" + env.get("PATH", "")
    if sb:
        env["PATH"] = ";".join(sb) + ";" + env.get("PATH", "")
    ml = cfg_list(cfg, "MSVC_LIB")
    sl = cfg_list(cfg, "WINSDK_LIB")
    if ml or sl:
        env["LIB"] = ";".join(ml + sl) + ";" + env.get("LIB", "")
    mi = cfg_list(cfg, "MSVC_INCLUDE")
    si = cfg_list(cfg, "WINSDK_INCLUDE")
    if mi or si:
        env["INCLUDE"] = ";".join(mi + si) + ";" + env.get("INCLUDE", "")
    vci = cfg_get(cfg, "VCINSTALLDIR")
    if vci:
        env["VCINSTALLDIR"] = vci
    else:
        # Auto derive VCINSTALLDIR from MSVC_BIN (…/VC/Tools/MSVC/…/bin/HostX64/x64)
        for b in mb:
            p = Path(b)
            cur = p
            while True:
                if cur.name.lower() == "vc":
                    env["VCINSTALLDIR"] = str(cur)
                    break
                if cur.parent == cur:
                    break
                cur = cur.parent
            if "VCINSTALLDIR" in env:
                break
    return env


def main() -> None:
    root = Path(__file__).resolve().parent
    repo = root.parent
    app_cfg = load_app_config(root)
    app_executable = read_app_value(app_cfg, "app_executable", "ImgcompressNative")
    build = root / "build"
    dist = root / "dist"
    cfg = load_config(root)
    global cfg_global
    cfg_global = cfg
    env = compose_env(cfg)
    cmake = resolve_tool("cmake", ["CMAKE_EXE", "CMAKE"], ["cmake.exe", "cmake", "cmake.bat", "cmake.cmd"])
    gen = resolve_generator_args(cmake, cfg)
    qt_prefix = resolve_qt_prefix(cfg)
    eg = None
    for i, v in enumerate(gen):
        if v == "-G" and i + 1 < len(gen):
            eg = gen[i + 1]
            break
    ensure_clean_build_dir(build, eg)
    extra: list[str] = []
    if eg == "Ninja":
        cxx = cfg_get(cfg, "CMAKE_CXX_COMPILER") or "cl"
        rc = cfg_get(cfg, "CMAKE_RC_COMPILER") or "rc"
        mt = cfg_get(cfg, "CMAKE_MT") or "mt"
        extra.extend(["-DCMAKE_CXX_COMPILER=" + cxx, "-DCMAKE_RC_COMPILER=" + rc, "-DCMAKE_MT=" + mt])
    run_command(
        [cmake, "-S", str(root), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release", *gen, *(["-DCMAKE_PREFIX_PATH=" + qt_prefix] if qt_prefix else []), *extra],
        root,
        env,
    )
    run_command([cmake, "--build", str(build), "--config", "Release"], root, env)
    exe = resolve_exe(build, app_executable)
    dist.mkdir(parents=True, exist_ok=True)
    out = dist / f"{app_executable}.exe"
    shutil.copy2(exe, out)
    wqt = resolve_tool("windeployqt", ["WINDEPLOYQT_EXE", "WINDEPLOYQT"], ["windeployqt.exe", "windeployqt", "windeployqt.bat", "windeployqt.cmd"])
    run_command([wqt, str(out)], root, env)
    vs = repo / "vendor"
    vd = dist / "vendor"
    if vs.is_dir():
        if vd.exists():
            shutil.rmtree(vd)
        shutil.copytree(vs, vd)
    print(f"已生成：{out}")


if __name__ == "__main__":
    main()
