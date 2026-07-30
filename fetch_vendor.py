from __future__ import annotations

import argparse
import io
import json
import os
import platform
import shutil
import stat
import sys
import tarfile
from pathlib import Path
from typing import Iterable, cast
from urllib.request import Request, urlopen
import subprocess


VENDOR_DIR = Path(__file__).resolve().parent / "vendor"
NPM_REGISTRY = os.environ.get("NPM_REGISTRY", "https://registry.npmmirror.com").rstrip("/")
BINARY_MIRROR = os.environ.get(
    "BINARY_MIRROR", "https://cdn.npmmirror.com/binaries"
).rstrip("/")
PLATFORMS = ["windows", "macos", "linux"]
ARCHS = ["x64", "arm64"]

TOOLS = {
    "pngquant": {
        "binary_names": ["pngquant"],
        "sources": [
            {"npm_package": "pngquant-bin", "mirror_name": "pngquant-bin"},
        ],
    },
    "oxipng": {
        "binary_names": ["oxipng"],
        "sources": [
            {"npm_package": "oxipng-bin", "mirror_name": "oxipng-bin"},
        ],
        # npm 的 macos 包只有 x86_64；arm64 / 官方构建走 GitHub Releases
        "github": {
            "repo": "oxipng/oxipng",
            "asset_markers": {
                "macos/arm64": ["aarch64-apple-darwin"],
                "macos/x64": ["x86_64-apple-darwin"],
                "linux/arm64": ["aarch64-unknown-linux-gnu"],
                "linux/x64": ["x86_64-unknown-linux-gnu"],
                "windows/x64": ["x86_64-pc-windows-msvc"],
            },
        },
    },
    "optipng": {
        "binary_names": ["optipng"],
        "sources": [
            {"npm_package": "optipng-bin", "mirror_name": "optipng-bin"},
        ],
    },
    "cjpeg": {
        "binary_names": ["cjpeg"],
        "sources": [
            {"npm_package": "mozjpeg", "mirror_name": "mozjpeg-bin"},
        ],
    },
    "jpegtran": {
        "binary_names": ["jpegtran"],
        "sources": [
            {"npm_package": "jpegtran-bin", "mirror_name": "jpegtran-bin"},
        ],
    },
    "gifsicle": {
        "binary_names": ["gifsicle"],
        "sources": [
            {"npm_package": "gifsicle", "mirror_name": "gifsicle-bin"},
        ],
    },
    "cwebp": {
        "binary_names": ["cwebp"],
        "sources": [
            {"npm_package": "cwebp-bin", "mirror_name": "cwebp-bin"},
        ],
    },
    #获取不到,注释掉
    # "dwebp": {
    #     "binary_names": ["dwebp"],
    #     "sources": [
    #         {"npm_package": "cwebp-bin", "mirror_name": "cwebp-bin"},
    #         {"npm_package": "cwebp", "mirror_name": "webp"},
    #     ],
    # },
}


def main() -> None:
    args = parse_args(sys.argv[1:])
    VENDOR_DIR.mkdir(parents=True, exist_ok=True)
    current_platform = detect_platform()
    current_arch = detect_arch()
    missing: list[str] = []
    npm_cache: dict[str, list[dict[str, object]]] = {}
    for name in args.tools:
        tool = TOOLS[name]
        for platform_key in args.platforms:
            for arch_key in args.archs:
                target_dir = VENDOR_DIR / platform_key / arch_key
                target_dir.mkdir(parents=True, exist_ok=True)
                used_fallback_arch = False
                payload = try_download_from_github(tool, platform_key, arch_key)
                source_label = "GitHub" if payload is not None else ""
                if payload is None:
                    if name not in npm_cache:
                        npm_cache[name] = load_npm_sources(tool)
                    sources = npm_cache[name]
                    for candidate_arch in arch_candidates(platform_key, arch_key):
                        for source in sources:
                            tar_bytes = cast(bytes, source["tar_bytes"])
                            with tarfile.open(
                                fileobj=io.BytesIO(tar_bytes), mode="r:gz"
                            ) as tar:
                                members = [
                                    m
                                    for m in tar.getmembers()
                                    if (m.isfile() or m.issym()) and "/vendor/" in m.name
                                ]
                                payload = fetch_payload_from_sources(
                                    tar,
                                    members,
                                    {
                                        "binary_names": tool["binary_names"],
                                        "mirror_name": source["mirror_name"],
                                    },
                                    cast(str, source["version"]),
                                    platform_key,
                                    candidate_arch,
                                )
                            if payload is not None:
                                used_fallback_arch = candidate_arch != arch_key
                                source_label = "npm"
                                break
                        if payload is not None:
                            break
                if payload is None and is_local_target(
                    platform_key, arch_key, current_platform, current_arch
                ):
                    copied = copy_from_system_for_arch(
                        cast(list[str], tool["binary_names"]),
                        target_dir,
                        platform_key,
                        arch_key,
                    )
                    if copied:
                        print(f"{name} -> {copied}")
                        continue
                if payload is None:
                    missing.append(f"{name} ({platform_key}/{arch_key})")
                    if not args.allow_missing:
                        raise RuntimeError(
                            f"未找到可用二进制：{name} ({platform_key}/{arch_key})"
                        )
                    print(f"{name} 缺失，已跳过：{platform_key}/{arch_key}")
                    continue
                target = target_dir / output_name(
                    cast(list[str], tool["binary_names"])[0], platform_key
                )
                write_bytes(target, payload)
                ensure_executable(target, platform_key)
                if platform_key == "macos" and not normalize_macos_binary(target, arch_key):
                    target.unlink(missing_ok=True)
                    missing.append(f"{name} ({platform_key}/{arch_key})")
                    if not args.allow_missing:
                        raise RuntimeError(
                            f"二进制架构不匹配：{name} ({platform_key}/{arch_key})"
                        )
                    print(f"{name} 架构不匹配，已跳过：{platform_key}/{arch_key}")
                    continue
                if used_fallback_arch:
                    print(f"{name} -> {target}（x64 回退）")
                elif source_label == "GitHub":
                    print(f"{name} -> {target}（GitHub）")
                else:
                    print(f"{name} -> {target}")
    if missing and not args.allow_missing:
        missing_text = "，".join(missing)
        raise RuntimeError(f"未找到可用二进制：{missing_text}")


def load_npm_sources(tool: dict[str, object]) -> list[dict[str, object]]:
    sources: list[dict[str, object]] = []
    for source in cast(list[dict[str, str]], tool.get("sources") or []):
        package = source.get("npm_package")
        if not package:
            continue
        tarball_url, package_version = resolve_npm_tarball(package)
        tar_bytes = download_bytes(tarball_url)
        with tarfile.open(fileobj=io.BytesIO(tar_bytes), mode="r:gz") as tar:
            version = read_package_version(tar) or package_version
        sources.append(
            {
                "tar_bytes": tar_bytes,
                "version": version,
                "mirror_name": source.get("mirror_name", ""),
            }
        )
    return sources


def expected_macho_arch(arch_key: str) -> str:
    if arch_key == "x64":
        return "x86_64"
    return arch_key


def lipo_arches(path: Path) -> list[str]:
    lipo = shutil.which("lipo")
    if not lipo:
        return []
    try:
        output = subprocess.check_output(
            [lipo, "-archs", str(path)], text=True, stderr=subprocess.DEVNULL
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return []
    return [item for item in output.split() if item]


def normalize_macos_binary(path: Path, arch_key: str) -> bool:
    target_arch = expected_macho_arch(arch_key)
    arches = lipo_arches(path)
    if not arches:
        return True
    if arches == [target_arch]:
        return True
    if target_arch not in arches:
        return False
    lipo = shutil.which("lipo")
    if not lipo:
        return False
    temp_path = path.with_name(path.name + ".thin_tmp")
    subprocess.check_call(
        [lipo, "-thin", target_arch, str(path), "-output", str(temp_path)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    temp_path.replace(path)
    return True


def parse_args(args: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("tools", nargs="*", help="pngquant oxipng optipng cjpeg jpegtran gifsicle cwebp")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--platforms", default="")
    parser.add_argument("--archs", default="")
    parser.add_argument("--allow-missing", action="store_true")
    parsed = parser.parse_args(args)
    tools = list(TOOLS.keys()) if parsed.all or not parsed.tools else parsed.tools
    unknown = [tool for tool in tools if tool not in TOOLS]
    if unknown:
        raise ValueError(f"未知工具：{', '.join(unknown)}")
    parsed.tools = tools
    parsed.platforms = normalize_targets(parsed.platforms, PLATFORMS, detect_platform())
    parsed.archs = normalize_targets(parsed.archs, ARCHS, detect_arch())
    return parsed


def normalize_targets(raw: str, allowed: list[str], default_value: str) -> list[str]:
    if not raw:
        return [default_value]
    if raw == "all":
        return allowed
    values = [item.strip() for item in raw.split(",") if item.strip()]
    unknown = [item for item in values if item not in allowed]
    if unknown:
        raise ValueError(f"未知平台或架构：{', '.join(unknown)}")
    return values


def select_binary(
    files: Iterable[tarfile.TarInfo],
    names: list[str],
    platform_key: str,
    arch_key: str,
) -> tarfile.TarInfo | None:
    candidates = [item for item in files if is_name_match(Path(item.name).name, names)]
    if not candidates:
        return None
    scored = [(score_candidate(item.name, platform_key, arch_key), item) for item in candidates]
    scored.sort(key=lambda item: item[0], reverse=True)
    return scored[0][1]


def fetch_payload_from_sources(
    tar: tarfile.TarFile,
    members: list[tarfile.TarInfo],
    tool: dict[str, object],
    version: str,
    platform_key: str,
    arch_key: str,
) -> bytes | None:
    selected = select_binary(
        members, cast(list[str], tool["binary_names"]), platform_key, arch_key
    )
    if selected:
        return extract_member_bytes(tar, selected)
    return try_download_from_mirror(tool, version, platform_key, arch_key)


def is_local_target(
    platform_key: str, arch_key: str, current_platform: str, current_arch: str
) -> bool:
    return platform_key == current_platform and arch_key == current_arch


def arch_candidates(platform_key: str, arch_key: str) -> list[str]:
    candidates = [arch_key]
    if platform_key == "windows" and arch_key == "arm64":
        candidates.append("x64")
    return candidates


def detect_platform() -> str:
    if sys.platform.startswith("win"):
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    return "linux"


def detect_arch() -> str:
    if hasattr(os, "uname"):
        machine = os.uname().machine.lower()
    else:
        machine = platform.machine().lower()
    if machine in {"arm64", "aarch64"}:
        return "arm64"
    if machine in {"x86_64", "amd64"}:
        return "x64"
    return machine


def score_candidate(text: str, platform_key: str, arch_key: str) -> int:
    text = text.lower()
    score = 0
    score += match_score(text, platform_tokens(platform_key)) * 3
    score += match_score(text, arch_tokens(arch_key)) * 2
    return score


def match_score(text: str, tokens: Iterable[str]) -> int:
    return sum(1 for token in tokens if token and token in text)


def platform_tokens(platform_key: str) -> list[str]:
    if platform_key == "windows":
        return ["win", "windows", "win32", "win64", "mingw", "msvc"]
    if platform_key == "macos":
        return ["mac", "macos", "darwin", "osx"]
    return ["linux", "gnu", "ubuntu", "debian", "centos"]


def arch_tokens(arch_key: str) -> list[str]:
    if arch_key == "arm64":
        return ["arm64", "aarch64", "arm"]
    if arch_key == "x64":
        return ["x64", "amd64", "x86_64"]
    return [arch_key]


def is_name_match(filename: str, names: Iterable[str]) -> bool:
    base = Path(filename).name
    stem = Path(filename).stem
    return any(base == name or stem == name for name in names)


def output_name(filename: str, platform_key: str) -> str:
    if platform_key == "windows" and not filename.endswith(".exe"):
        return f"{filename}.exe"
    if platform_key != "windows" and filename.endswith(".exe"):
        return Path(filename).stem
    return filename


def write_bytes(target: Path, data: bytes) -> None:
    temp = target.with_suffix(target.suffix + ".partial")
    temp.write_bytes(data)
    temp.replace(target)


def extract_member_bytes(
    tar: tarfile.TarFile, member: tarfile.TarInfo, depth: int = 0
) -> bytes | None:
    if depth > 5:
        return None
    if member.isfile():
        payload = tar.extractfile(member)
        return payload.read() if payload else None
    if member.issym():
        resolved = resolve_symlink_member(tar, member)
        if resolved is None:
            return None
        return extract_member_bytes(tar, resolved, depth + 1)
    return None


def resolve_symlink_member(
    tar: tarfile.TarFile, member: tarfile.TarInfo
) -> tarfile.TarInfo | None:
    if not member.linkname:
        return None
    if member.linkname.startswith("http"):
        return None
    if member.linkname.startswith("package/"):
        link_target = member.linkname
    else:
        base = Path(member.name).parent
        link_target = str((base / member.linkname).as_posix())
    if link_target.startswith("./"):
        link_target = link_target[2:]
    try:
        return tar.getmember(link_target)
    except KeyError:
        return None


def ensure_executable(path: Path, platform_key: str) -> None:
    if platform_key == "windows":
        return
    mode = path.stat().st_mode
    path.chmod(mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


# Prefer Homebrew mozjpeg over PATH cjpeg/jpegtran (often libjpeg-turbo).
MACOS_PREFERRED_TOOL_PATHS: dict[str, list[str]] = {
    "cjpeg": [
        "/opt/homebrew/opt/mozjpeg/bin/cjpeg",
        "/usr/local/opt/mozjpeg/bin/cjpeg",
    ],
    "jpegtran": [
        "/opt/homebrew/opt/mozjpeg/bin/jpegtran",
        "/usr/local/opt/mozjpeg/bin/jpegtran",
    ],
}


def resolve_system_tool_path(name: str) -> Path | None:
    preferred = MACOS_PREFERRED_TOOL_PATHS.get(name, [])
    for candidate in preferred:
        path = Path(candidate)
        if path.is_file():
            return path
    system_path = shutil.which(name)
    if system_path:
        return Path(system_path)
    return None


def copy_from_system(names: list[str], target_dir: Path, platform_key: str) -> Path | None:
    for name in names:
        source = resolve_system_tool_path(name)
        if source is None:
            continue
        target = target_dir / output_name(source.name, platform_key)
        write_bytes(target, source.read_bytes())
        ensure_executable(target, platform_key)
        return target
    return None


def is_macos_system_dylib(dep: str) -> bool:
    return (
        dep.startswith("/usr/lib/")
        or dep.startswith("/System/")
        or dep.startswith("/Library/Apple/")
    )


def parse_otool_deps(binary: Path) -> list[str]:
    try:
        out = subprocess.check_output(["otool", "-L", str(binary)], text=True)
    except (OSError, subprocess.CalledProcessError):
        return []
    deps: list[str] = []
    for line in out.splitlines()[1:]:
        dep = line.strip().split(" ", 1)[0]
        if dep and not dep.endswith(":"):
            deps.append(dep)
    return deps


def macos_dylib_search_paths(name: str, binary: Path) -> list[Path]:
    homebrew_roots = [Path("/opt/homebrew/opt"), Path("/usr/local/opt")]
    candidates: list[Path] = []
    for root in homebrew_roots:
        if root.is_dir():
            for opt_lib in sorted(root.glob("*/lib")):
                candidates.append(opt_lib / name)
        candidates.extend(
            [
                root.parent / "lib" / name,
                Path("/opt/homebrew/lib") / name,
                Path("/usr/local/lib") / name,
            ]
        )
    parent = binary.resolve().parent
    candidates.extend(
        [
            parent / name,
            parent / "lib" / name,
            parent.parent / "lib" / name,
        ]
    )
    return candidates


def find_macos_dylib(name: str, binary: Path) -> Path | None:
    for src in macos_dylib_search_paths(name, binary):
        if src.is_file():
            return src.resolve()
    return None


def rewrite_macos_install_names(path: Path, lib_dir: Path) -> None:
    """Point non-system absolute / @rpath deps at @rpath/<basename> when vendored."""
    install_name_tool = shutil.which("install_name_tool")
    if not install_name_tool:
        return
    try:
        path.chmod(path.stat().st_mode | stat.S_IWUSR)
    except OSError:
        pass
    if path.suffix == ".dylib":
        try:
            subprocess.check_call(
                [install_name_tool, "-id", f"@rpath/{path.name}", str(path)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except subprocess.CalledProcessError:
            pass
    for dep in parse_otool_deps(path):
        if dep.startswith("@") and not dep.startswith("@rpath/"):
            continue
        if is_macos_system_dylib(dep):
            continue
        name = Path(dep).name
        if not (lib_dir / name).is_file():
            continue
        if dep == f"@rpath/{name}":
            continue
        try:
            subprocess.check_call(
                [install_name_tool, "-change", dep, f"@rpath/{name}", str(path)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except subprocess.CalledProcessError:
            continue
    codesign = shutil.which("codesign")
    if codesign:
        subprocess.call(
            [codesign, "--force", "-s", "-", str(path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )


def copy_macos_loader_libs(binary: Path, vendor_macos_dir: Path) -> None:
    """Copy @rpath and non-system absolute dylibs into vendor/macos/lib.

    Also copies transitive deps and rewrites install names to @rpath so binaries
    run with @loader_path/../lib without DYLD_LIBRARY_PATH / Homebrew.
    """
    lib_dir = vendor_macos_dir / "lib"
    pending = [binary.resolve()]
    seen_bins: set[Path] = set()
    copied_names: set[str] = set()

    while pending:
        current = pending.pop()
        if current in seen_bins:
            continue
        seen_bins.add(current)
        for dep in parse_otool_deps(current):
            if is_macos_system_dylib(dep):
                continue
            # Vendor @rpath and absolute non-system deps; skip other @ paths.
            if dep.startswith("@rpath/"):
                pass
            elif dep.startswith("@"):
                continue
            elif not dep.startswith("/"):
                continue
            name = Path(dep).name
            if not name.endswith(".dylib"):
                continue
            dest = lib_dir / name
            if name not in copied_names:
                src = find_macos_dylib(name, binary)
                if src is None and dep.startswith("/") and Path(dep).is_file():
                    src = Path(dep).resolve()
                if src is None:
                    print(f"warn: missing dylib for {dep}")
                    continue
                lib_dir.mkdir(parents=True, exist_ok=True)
                write_bytes(dest, src.read_bytes())
                print(f"lib -> {dest}")
                copied_names.add(name)
                pending.append(dest)
            if dest.is_file() and dest not in seen_bins:
                pending.append(dest)

    # Rewrite binary + all vendored libs to use @rpath names.
    rewrite_macos_install_names(binary, lib_dir)
    for name in sorted(copied_names):
        rewrite_macos_install_names(lib_dir / name, lib_dir)

    # Ensure @loader_path/../lib rpath exists on the binary.
    try:
        out = subprocess.check_output(["otool", "-l", str(binary)], text=True)
    except (OSError, subprocess.CalledProcessError):
        return
    if "@loader_path/../lib" in out:
        return
    install_name_tool = shutil.which("install_name_tool")
    if not install_name_tool:
        return
    try:
        binary.chmod(binary.stat().st_mode | stat.S_IWUSR)
        subprocess.check_call(
            [install_name_tool, "-add_rpath", "@loader_path/../lib", str(binary)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except (OSError, subprocess.CalledProcessError):
        return
    codesign = shutil.which("codesign")
    if codesign:
        subprocess.call(
            [codesign, "--force", "-s", "-", str(binary)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )


def copy_from_system_for_arch(
    names: list[str], target_dir: Path, platform_key: str, arch_key: str
) -> Path | None:
    copied = copy_from_system(names, target_dir, platform_key)
    if copied is None:
        return None
    if platform_key == "macos" and not normalize_macos_binary(copied, arch_key):
        copied.unlink(missing_ok=True)
        return None
    if platform_key == "macos":
        # target_dir is vendor/macos/<arch>
        copy_macos_loader_libs(copied, target_dir.parent)
    return copied


def resolve_npm_tarball(package: str) -> tuple[str, str]:
    url = f"{NPM_REGISTRY}/{package}/latest"
    payload = json_get(url)
    dist = payload.get("dist") or {}
    tarball = dist.get("tarball")
    if not tarball:
        raise RuntimeError(f"无法获取 tarball 地址：{package}")
    version = payload.get("version") or ""
    return tarball, version


def read_package_version(tar: tarfile.TarFile) -> str:
    try:
        member = tar.getmember("package/package.json")
    except KeyError:
        return ""
    payload = tar.extractfile(member)
    if payload is None:
        return ""
    try:
        data = json.loads(payload.read().decode("utf-8"))
    except json.JSONDecodeError:
        return ""
    return data.get("version", "")


def try_download_from_github(
    tool: dict[str, object],
    platform_key: str,
    arch_key: str,
) -> bytes | None:
    github = cast("dict[str, object] | None", tool.get("github"))
    if not github:
        return None
    repo = cast("str | None", github.get("repo"))
    markers_map = cast("dict[str, list[str]] | None", github.get("asset_markers"))
    if not repo or not markers_map:
        return None
    markers = markers_map.get(f"{platform_key}/{arch_key}")
    if not markers:
        return None
    names = cast(list[str], tool["binary_names"])
    release_urls = [
        f"https://api.github.com/repos/{repo}/releases/latest",
        f"https://ghproxy.net/https://api.github.com/repos/{repo}/releases/latest",
    ]
    release = None
    for url in release_urls:
        try:
            release = json_get(url)
            break
        except Exception:
            continue
    if not release:
        return None
    assets = cast(list[dict], release.get("assets") or [])
    asset = None
    for item in assets:
        name = str(item.get("name") or "").lower()
        if all(marker.lower() in name for marker in markers):
            asset = item
            break
    if asset is None:
        return None
    download_url = str(asset.get("browser_download_url") or "")
    if not download_url:
        return None
    candidate_urls = [download_url, f"https://ghproxy.net/{download_url}"]
    archive = None
    for url in candidate_urls:
        try:
            archive = download_bytes(url)
            break
        except Exception:
            continue
    if archive is None:
        return None
    asset_name = str(asset.get("name") or "")
    return extract_binary_from_archive(archive, asset_name, names)


def extract_binary_from_archive(
    archive: bytes, asset_name: str, names: list[str]
) -> bytes | None:
    lower_name = asset_name.lower()
    if lower_name.endswith(".tar.gz") or lower_name.endswith(".tgz"):
        with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as tar:
            members = [m for m in tar.getmembers() if m.isfile()]
            selected = None
            for member in members:
                if is_name_match(Path(member.name).name, names):
                    selected = member
                    break
            if selected is None:
                return None
            return extract_member_bytes(tar, selected)
    if lower_name.endswith(".zip"):
        import zipfile

        with zipfile.ZipFile(io.BytesIO(archive)) as zf:
            for info in zf.infolist():
                if info.is_dir():
                    continue
                if is_name_match(Path(info.filename).name, names):
                    return zf.read(info.filename)
        return None
    # bare binary asset
    if any(is_name_match(Path(asset_name).name, [name]) for name in names):
        return archive
    return None


def try_download_from_mirror(
    tool: dict[str, object],
    version: str,
    platform_key: str,
    arch_key: str,
) -> bytes | None:
    mirror_name = cast("str | None", tool.get("mirror_name"))
    if not mirror_name or not version:
        return None
    base = f"{BINARY_MIRROR}/{mirror_name}/v{version}"
    binary_name = cast(list[str], tool["binary_names"])[0]
    binary_name_windows = f"{binary_name}.exe"
    candidates = [
        f"{base}/vendor/{platform_key}/{arch_key}/{binary_name}",
        f"{base}/vendor/{platform_key}/{binary_name}",
    ]
    if platform_key == "macos":
        candidates.extend(
            [
                f"{base}/vendor/darwin/{arch_key}/{binary_name}",
                f"{base}/vendor/darwin/{binary_name}",
                f"{base}/vendor/osx/{arch_key}/{binary_name}",
                f"{base}/vendor/osx/{binary_name}",
            ]
        )
    if platform_key == "windows":
        candidates.extend(
            [
                f"{base}/vendor/{platform_key}/{arch_key}/{binary_name_windows}",
                f"{base}/vendor/{platform_key}/{binary_name_windows}",
                f"{base}/vendor/win32/{arch_key}/{binary_name_windows}",
                f"{base}/vendor/win32/{binary_name_windows}",
                f"{base}/vendor/win/{arch_key}/{binary_name_windows}",
                f"{base}/vendor/win/{binary_name_windows}",
            ]
        )
    if platform_key == "linux":
        candidates.extend(
            [
                f"{base}/vendor/linux/{arch_key}/{binary_name}",
                f"{base}/vendor/linux/{binary_name}",
            ]
        )
    for url in candidates:
        try:
            return download_bytes(url)
        except Exception:
            continue
    return None


def download_bytes(url: str) -> bytes:
    request = Request(url, headers={"User-Agent": "imgcompress-fetcher"})
    with urlopen(request, timeout=60) as response:
        return response.read()


def json_get(url: str) -> dict:
    request = Request(url, headers={"User-Agent": "imgcompress-fetcher"})
    with urlopen(request, timeout=30) as response:
        payload = response.read().decode("utf-8")
    return json.loads(payload)


if __name__ == "__main__":
    main()
