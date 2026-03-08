from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile


def run_command(command: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
    process = subprocess.Popen(
        command,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        universal_newlines=True,
        env=env,
    )
    stdout = process.stdout
    if stdout is not None:
        for line in stdout:
            print(line, end="")
    process.wait()
    if process.returncode != 0:
        raise SystemExit(process.returncode)


def load_app_config(root: Path) -> dict:
    cfg_path = root / "app_config.json"
    if cfg_path.exists():
        try:
            return json.loads(cfg_path.read_text(encoding="utf-8"))
        except Exception:
            return {}
    return {}


def load_config(root: Path) -> dict:
    cfg = {}
    ext = os.environ.get("IMG_COMPRESS_CONFIG")
    if ext and Path(ext).exists():
        try:
            cfg = json.loads(Path(ext).read_text(encoding="utf-8"))
        except Exception:
            cfg = {}
    else:
        cfg_path = root / "build_config.macos.json"
        if cfg_path.exists():
            try:
                cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
            except Exception:
                cfg = {}
    return cfg


def read_app_value(cfg: dict, key: str, default: str) -> str:
    v = cfg.get(key)
    return str(v) if v else default


def cfg_get(cfg: dict, key: str) -> str | None:
    v = cfg.get(key)
    if v is None:
        return None
    if isinstance(v, list):
        items = [str(item) for item in v if item]
        return ";".join(items) if items else None
    return str(v)


def normalize_path(value: str) -> str:
    return str(Path(value).expanduser())


def resolve_cmake(cfg: dict, env: dict[str, str]) -> str:
    cmake_value = cfg_get(cfg, "CMAKE_EXE") or cfg_get(cfg, "CMAKE") or env.get("CMAKE")
    if cmake_value:
        cmake_path = normalize_path(cmake_value)
        if Path(cmake_path).exists():
            return cmake_path
    cmake = shutil.which("cmake")
    if cmake:
        return cmake
    raise SystemExit("未找到 cmake")


def resolve_qt_prefix(cfg: dict, env: dict[str, str]) -> str | None:
    for key in ("QT_PREFIX", "CMAKE_PREFIX_PATH"):
        value = cfg_get(cfg, key) or env.get(key)
        if value:
            return normalize_path(value)
    return None


def resolve_qt_dir(qt_prefix: str | None) -> str | None:
    if not qt_prefix:
        return None
    qt_dir = Path(qt_prefix) / "lib" / "cmake" / "Qt6"
    if qt_dir.exists():
        return str(qt_dir)
    return None


def resolve_macdeployqt(cfg: dict, env: dict[str, str], qt_prefix: str | None) -> str:
    deploy_env = cfg_get(cfg, "MACDEPLOYQT_EXE") or env.get("MACDEPLOYQT_EXE")
    if deploy_env:
        deploy_path = normalize_path(deploy_env)
        if Path(deploy_path).exists():
            return deploy_path
    candidates = []
    if qt_prefix:
        candidates.append(Path(qt_prefix) / "bin" / "macdeployqt")
    candidates.append(shutil.which("macdeployqt"))
    for item in candidates:
        if not item:
            continue
        path = Path(item)
        if path.exists():
            return str(path)
    raise SystemExit("未找到 macdeployqt")


def resolve_app(build_dir: Path, app_executable: str) -> Path:
    app_name = f"{app_executable}.app"
    candidates = [
        build_dir / app_name,
        build_dir / "Release" / app_name,
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise SystemExit(f"未找到 {app_name}")


def resolve_qtpaths(cfg: dict, env: dict[str, str], qt_prefix: str | None) -> str:
    qtpaths_env = cfg_get(cfg, "QT_PATHS") or env.get("QT_PATHS")
    if qtpaths_env:
        return normalize_path(qtpaths_env)
    candidates = []
    if qt_prefix:
        candidates.append(Path(qt_prefix) / "bin" / "qtpaths")
        candidates.append(Path(qt_prefix) / "bin" / "qtpaths6")
    candidates.extend(
        [
            shutil.which("qtpaths"),
            shutil.which("qtpaths6"),
        ]
    )
    for item in candidates:
        if not item:
            continue
        path = Path(item)
        if path.exists():
            return str(path)
    raise SystemExit("未找到 qtpaths 或 qtpaths6")


def read_qt_plugin_dir(env: dict[str, str], cfg: dict, qt_prefix: str | None) -> Path:
    qtpaths = resolve_qtpaths(cfg, env, qt_prefix)
    output = subprocess.check_output([qtpaths, "--plugin-dir"], text=True, env=env).strip()
    if not output:
        raise SystemExit("无法获取 Qt 插件目录")
    return Path(output)


def copy_plugin_files(plugin_dir: Path, app_path: Path, group: str, names: list[str]) -> None:
    src_dir = plugin_dir / group
    if not src_dir.exists():
        return
    dest_dir = app_path / "Contents" / "PlugIns" / group
    dest_dir.mkdir(parents=True, exist_ok=True)
    for name in names:
        src = src_dir / name
        if src.exists():
            shutil.copy2(src, dest_dir / name)


def copy_app(src: Path, dst: Path) -> None:
    shutil.copytree(src, dst, symlinks=True, copy_function=shutil.copy2)


def resolve_codesign(env: dict[str, str]) -> str:
    codesign = shutil.which("codesign")
    if codesign is None:
        raise SystemExit("未找到 codesign")
    return codesign


def build_codesign_args(identity: str) -> list[str]:
    args = ["--force", "--sign", identity]
    if identity == "-":
        args.append("--timestamp=none")
        return args
    return [*args, "--timestamp", "--options", "runtime"]


def sign_item(path: Path, env: dict[str, str]) -> None:
    codesign = resolve_codesign(env)
    identity = env.get("SIGN_IDENTITY", "-")
    run_command(
        [
            codesign,
            *build_codesign_args(identity),
            str(path),
        ],
        path.parent,
        env,
    )


def is_macho_file(path: Path) -> bool:
    try:
        with path.open("rb") as handle:
            magic = handle.read(4)
    except OSError:
        return False
    return magic in {
        b"\xfe\xed\xfa\xce",
        b"\xfe\xed\xfa\xcf",
        b"\xcf\xfa\xed\xfe",
        b"\xca\xfe\xba\xbe",
        b"\xbe\xba\xfe\xca",
        b"\xca\xfe\xba\xbf",
        b"\xbf\xba\xfe\xca",
    }


def iter_macho_files(app_path: Path):
    seen: set[str] = set()
    for root, _, files in os.walk(app_path):
        for name in files:
            path = Path(root) / name
            if any(part.endswith(".framework") for part in path.parts):
                continue
            real_path = path
            if path.is_symlink():
                try:
                    real_path = path.resolve()
                except OSError:
                    continue
            if not real_path.exists() or not real_path.is_file():
                continue
            if is_macho_file(real_path):
                key = str(real_path)
                if key in seen:
                    continue
                seen.add(key)
                yield real_path


def deploy_qt_plugins(app_path: Path, env: dict[str, str], cfg: dict, qt_prefix: str | None) -> None:
    plugin_dir = read_qt_plugin_dir(env, cfg, qt_prefix)
    copy_plugin_files(plugin_dir, app_path, "platforms", ["libqcocoa.dylib"])
    copy_plugin_files(
        plugin_dir,
        app_path,
        "imageformats",
        ["libqjpeg.dylib", "libqpng.dylib", "libqgif.dylib", "libqwebp.dylib"],
    )
    copy_plugin_files(plugin_dir, app_path, "styles", ["libqmacstyle.dylib"])


def deploy_vendor(app_path: Path, repo_dir: Path) -> None:
    resources_dir = app_path / "Contents" / "Resources"
    resources_dir.mkdir(parents=True, exist_ok=True)
    vendor_src = repo_dir / "vendor"
    vendor_dst = resources_dir / "vendor"
    if vendor_src.is_dir():
        if vendor_dst.exists():
            shutil.rmtree(vendor_dst)
        shutil.copytree(vendor_src, vendor_dst)


def has_framework_info(framework_path: Path) -> bool:
    return (framework_path / "Resources" / "Info.plist").exists() or (
        framework_path / "Versions" / "Current" / "Resources" / "Info.plist"
    ).exists()


def sign_app(app_path: Path, env: dict[str, str]) -> None:
    frameworks_dir = app_path / "Contents" / "Frameworks"
    if frameworks_dir.exists():
        for item in frameworks_dir.iterdir():
            if item.suffix == ".framework":
                framework_binary = item / "Versions" / "Current" / item.stem
                if framework_binary.exists():
                    try:
                        framework_binary = framework_binary.resolve()
                    except OSError:
                        pass
                    sign_item(framework_binary, env)
                if has_framework_info(item):
                    sign_item(item, env)
    targets = sorted(iter_macho_files(app_path), key=lambda item: str(item))
    for path in targets:
        sign_item(path, env)
    sign_item(app_path, env)


def build_dmg(
    staging_dir: Path,
    dist_dir: Path,
    env: dict[str, str],
    dmg_name: str,
    volume_name: str,
) -> Path:
    if shutil.which("hdiutil") is None:
        raise SystemExit("未找到 hdiutil")
    dmg_path = dist_dir / f"{dmg_name}.dmg"
    if dmg_path.exists():
        dmg_path.unlink()
    run_command(
        [
            "hdiutil",
            "create",
            "-volname",
            volume_name,
            "-srcfolder",
            str(staging_dir),
            "-ov",
            "-format",
            "UDZO",
            str(dmg_path),
        ],
        dist_dir,
        env,
    )
    if not dmg_path.exists():
        raise SystemExit("dmg 生成失败")
    return dmg_path


def resolve_build_settings(cfg: dict, env: dict[str, str]) -> dict[str, str | None]:
    cmake = resolve_cmake(cfg, env)
    qt_prefix = resolve_qt_prefix(cfg, env)
    qt_dir = resolve_qt_dir(qt_prefix)
    macdeployqt = resolve_macdeployqt(cfg, env, qt_prefix)
    sysroot = cfg_get(cfg, "CMAKE_OSX_SYSROOT") or env.get("CMAKE_OSX_SYSROOT")
    if not sysroot:
        sysroot = "macosx"
    deployment_target = cfg_get(cfg, "CMAKE_OSX_DEPLOYMENT_TARGET") or env.get(
        "CMAKE_OSX_DEPLOYMENT_TARGET"
    )
    if not deployment_target:
        deployment_target = "11.0"
    architectures = cfg_get(cfg, "CMAKE_OSX_ARCHITECTURES") or env.get("CMAKE_OSX_ARCHITECTURES")
    if not architectures:
        architectures = "arm64;x86_64"
    return {
        "cmake": cmake,
        "qt_prefix": qt_prefix,
        "qt_dir": qt_dir,
        "macdeployqt": macdeployqt,
        "sysroot": sysroot,
        "deployment_target": deployment_target,
        "architectures": architectures,
    }


def build_cmake_args(root_dir: Path, build_dir: Path, settings: dict[str, str | None]) -> list[str]:
    args = [
        str(settings["cmake"]),
        "-S",
        str(root_dir),
        "-B",
        str(build_dir),
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_OSX_SYSROOT=" + str(settings["sysroot"]),
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=" + str(settings["deployment_target"]),
        "-DCMAKE_OSX_ARCHITECTURES=" + str(settings["architectures"]),
    ]
    if settings["qt_prefix"]:
        args.append("-DCMAKE_PREFIX_PATH=" + str(settings["qt_prefix"]))
    if settings["qt_dir"]:
        args.append("-DQt6_DIR=" + str(settings["qt_dir"]))
    return args


def package_for_arch(
    root_dir: Path,
    repo_dir: Path,
    app_name: str,
    app_executable: str,
    dist_dir: Path,
    env: dict[str, str],
    cfg: dict,
    settings: dict[str, str | None],
    arch: str,
    build_dir: Path,
    dmg_suffix: str,
    split_output: bool,
) -> None:
    arch_settings = dict(settings)
    arch_settings["architectures"] = arch
    run_command(build_cmake_args(root_dir, build_dir, arch_settings), root_dir, env)
    run_command([str(arch_settings["cmake"]), "--build", str(build_dir), "--config", "Release"], root_dir, env)
    app_path = resolve_app(build_dir, app_executable)
    run_command([str(arch_settings["macdeployqt"]), str(app_path), "-no-plugins"], root_dir, env)
    deploy_qt_plugins(app_path, env, cfg, arch_settings["qt_prefix"])
    deploy_vendor(app_path, repo_dir)
    output_dir = dist_dir / arch if split_output else dist_dir
    output_dir.mkdir(parents=True, exist_ok=True)
    dist_app = output_dir / f"{app_executable}.app"
    if dist_app.exists():
        shutil.rmtree(dist_app)
    copy_app(app_path, dist_app)
    sign_app(dist_app, env)
    staging_dir = Path(tempfile.mkdtemp(prefix=f"imgcompress_native_dmg_{arch}_"))
    keep_staging = env.get("KEEP_STAGING", "").lower() in {"1", "true", "yes"}
    try:
        app_target = staging_dir / f"{app_executable}.app"
        copy_app(dist_app, app_target)
        apps_link = staging_dir / "Applications"
        if apps_link.exists():
            apps_link.unlink()
        os.symlink("/Applications", apps_link)
        dmg_path = build_dmg(staging_dir, dist_dir, env, f"{app_name}{dmg_suffix}", app_name)
        print(f"已生成：{dist_app}")
        print(f"已生成：{dmg_path}")
    finally:
        if not keep_staging and staging_dir.exists():
            shutil.rmtree(staging_dir)


def main() -> None:
    root_dir = Path(__file__).resolve().parent
    repo_dir = root_dir.parent
    cfg = load_config(root_dir)
    app_cfg = load_app_config(root_dir)
    app_name = read_app_value(app_cfg, "app_name", "Imgcompress")
    app_executable = read_app_value(app_cfg, "app_executable", "ImgcompressNative")
    build_dir = root_dir / "build"
    dist_dir = root_dir / "dist"
    env = dict(os.environ)
    settings = resolve_build_settings(cfg, env)
    split_raw = cfg_get(cfg, "SPLIT_ARCH_PACKAGES") or env.get("SPLIT_ARCH_PACKAGES")
    split_arch = str(split_raw).strip().lower() in {"1", "true", "yes", "on"}
    arch_list = [item.strip() for item in str(settings["architectures"]).replace(",", ";").split(";") if item.strip()]
    if split_arch and len(arch_list) > 1:
        for arch in arch_list:
            package_for_arch(
                root_dir,
                repo_dir,
                app_name,
                app_executable,
                dist_dir,
                env,
                cfg,
                settings,
                arch,
                root_dir / f"build_{arch}",
                f"-{arch}",
                True,
            )
        return
    package_for_arch(
        root_dir,
        repo_dir,
        app_name,
        app_executable,
        dist_dir,
        env,
        cfg,
        settings,
        arch_list[0] if arch_list else "arm64",
        root_dir / "build",
        "",
        False,
    )


if __name__ == "__main__":
    main()
