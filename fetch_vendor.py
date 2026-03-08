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
    for name in args.tools:
        tool = TOOLS[name]
        sources = []
        for source in cast(list[dict[str, str]], tool["sources"]):
            package = source["npm_package"]
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
        for platform_key in args.platforms:
            for arch_key in args.archs:
                target_dir = VENDOR_DIR / platform_key / arch_key
                target_dir.mkdir(parents=True, exist_ok=True)
                used_fallback_arch = False
                payload = None
                for candidate_arch in arch_candidates(platform_key, arch_key):
                    for source in sources:
                        with tarfile.open(
                            fileobj=io.BytesIO(source["tar_bytes"]), mode="r:gz"
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
                                source["version"],
                                platform_key,
                                candidate_arch,
                            )
                        if payload is not None:
                            used_fallback_arch = candidate_arch != arch_key
                            break
                    if payload is not None:
                        break
                if payload is None and is_local_target(
                    platform_key, arch_key, current_platform, current_arch
                ):
                    copied = copy_from_system(
                        cast(list[str], tool["binary_names"]), target_dir, platform_key
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
                if used_fallback_arch:
                    print(f"{name} -> {target}（x64 回退）")
                else:
                    print(f"{name} -> {target}")
    if missing and not args.allow_missing:
        missing_text = "，".join(missing)
        raise RuntimeError(f"未找到可用二进制：{missing_text}")


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


def copy_from_system(names: list[str], target_dir: Path, platform_key: str) -> Path | None:
    for name in names:
        system_path = shutil.which(name)
        if not system_path:
            continue
        source = Path(system_path)
        target = target_dir / output_name(source.name, platform_key)
        write_bytes(target, source.read_bytes())
        ensure_executable(target, platform_key)
        return target
    return None


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


def try_download_from_mirror(
    tool: dict[str, object],
    version: str,
    platform_key: str,
    arch_key: str,
) -> bytes | None:
    mirror_name = cast(str | None, tool.get("mirror_name"))
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
    with urlopen(request) as response:
        return response.read()


def json_get(url: str) -> dict:
    request = Request(url, headers={"User-Agent": "imgcompress-fetcher"})
    with urlopen(request) as response:
        payload = response.read().decode("utf-8")
    return json.loads(payload)


if __name__ == "__main__":
    main()
