from pathlib import Path
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


def copy_app(src: Path, dst: Path) -> None:
    shutil.copytree(src, dst, symlinks=True, copy_function=shutil.copy2)


def resolve_qtpaths(env: dict[str, str]) -> str:
    qtpaths_env = env.get("QT_PATHS")
    if qtpaths_env:
        return qtpaths_env
    candidates = [
        shutil.which("qtpaths"),
        shutil.which("qtpaths6"),
    ]
    for item in candidates:
        if item and Path(item).exists():
            return str(item)
    raise SystemExit("未找到 qtpaths 或 qtpaths6")


def read_qt_plugin_dir(env: dict[str, str]) -> Path:
    qtpaths = resolve_qtpaths(env)
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


def deploy_qt_plugins(app_path: Path, env: dict[str, str]) -> None:
    plugin_dir = read_qt_plugin_dir(env)
    copy_plugin_files(plugin_dir, app_path, "platforms", ["libqcocoa.dylib"])
    copy_plugin_files(
        plugin_dir,
        app_path,
        "imageformats",
        ["libqjpeg.dylib", "libqpng.dylib", "libqgif.dylib", "libqwebp.dylib"],
    )
    copy_plugin_files(plugin_dir, app_path, "styles", ["libqmacstyle.dylib"])


def write_qt_conf(app_path: Path) -> None:
    resources_dir = app_path / "Contents" / "Resources"
    resources_dir.mkdir(parents=True, exist_ok=True)
    qt_conf = resources_dir / "qt.conf"
    qt_conf.write_text("[Paths]\nPlugins=PlugIns\nLibraries=Frameworks\n", encoding="utf-8")


def prune_internal_apps(app_path: Path) -> None:
    for path in app_path.rglob("*.app"):
        if path == app_path:
            continue
        remove_path(path)
    for path in app_path.rglob("*__dot__app"):
        if path == app_path:
            continue
        remove_path(path)


def remove_path(path: Path) -> None:
    if path.is_symlink():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)


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


def has_framework_info(framework_path: Path) -> bool:
    return (framework_path / "Resources" / "Info.plist").exists() or (
        framework_path / "Versions" / "Current" / "Resources" / "Info.plist"
    ).exists()


def sign_frameworks(app_path: Path, env: dict[str, str]) -> None:
    frameworks_root = app_path / "Contents" / "Frameworks"
    if not frameworks_root.exists():
        return
    for framework in sorted(frameworks_root.rglob("*.framework"), key=lambda item: str(item)):
        if not framework.is_dir():
            continue
        framework_binary = framework / "Versions" / "Current" / framework.stem
        if framework_binary.exists():
            try:
                framework_binary = framework_binary.resolve()
            except OSError:
                pass
            sign_item(framework_binary, env)
        if has_framework_info(framework):
            sign_item(framework, env)


def sign_app(app_path: Path, env: dict[str, str]) -> None:
    sign_frameworks(app_path, env)
    targets = sorted(iter_macho_files(app_path), key=lambda item: str(item))
    for path in targets:
        sign_item(path, env)
    sign_item(app_path, env)


def main() -> None:
    root_dir = Path(__file__).resolve().parent
    build_py = root_dir / "build.py"
    print("开始 PyInstaller 打包...")
    run_command([sys.executable, str(build_py)], root_dir, dict(os.environ))
    dist_app = root_dir / "dist" / "Imgcompress.app"
    if not dist_app.exists():
        raise SystemExit(f"未找到应用包：{dist_app}")
    deploy_qt_plugins(dist_app, dict(os.environ))
    write_qt_conf(dist_app)
    prune_internal_apps(dist_app)
    print("开始 codesign...")
    sign_app(dist_app, dict(os.environ))
    staging_dir = Path(tempfile.mkdtemp(prefix="imgcompress_dmg_"))
    app_target = staging_dir / "Imgcompress.app"
    copy_app(dist_app, app_target)
    apps_link = staging_dir / "Applications"
    if apps_link.exists():
        apps_link.unlink()
    os.symlink("/Applications", apps_link)
    dmg_path = root_dir / "dist" / "Imgcompress.dmg"
    if dmg_path.exists():
        dmg_path.unlink()
    print("开始制作 dmg...")
    run_command(
        [
            "hdiutil",
            "create",
            "-volname",
            "Imgcompress",
            "-srcfolder",
            str(staging_dir),
            "-ov",
            "-format",
            "UDZO",
            str(dmg_path),
        ],
        root_dir,
        dict(os.environ),
    )
    shutil.rmtree(staging_dir)
    pkg_path = root_dir / "dist" / "Imgcompress.pkg"
    if pkg_path.exists():
        pkg_path.unlink()
    print("开始制作 pkg...")
    run_command(
        [
            "pkgbuild",
            "--install-location",
            "/Applications",
            "--component",
            str(dist_app),
            str(pkg_path),
        ],
        root_dir,
        dict(os.environ),
    )
    print(f"已生成：{dmg_path}")
    print(f"已生成：{pkg_path}")


if __name__ == "__main__":
    main()
