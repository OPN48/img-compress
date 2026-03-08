from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--svg")
    parser.add_argument("--icns")
    parser.add_argument("--ico")
    return parser.parse_args()


def resolve_svg_renderer() -> tuple[str, str]:
    rsvg = shutil.which("rsvg-convert")
    if rsvg:
        return "rsvg", rsvg
    inkscape = shutil.which("inkscape")
    if inkscape:
        return "inkscape", inkscape
    raise SystemExit("未找到 rsvg-convert 或 inkscape")


def render_png(renderer: tuple[str, str], svg_path: Path, size: int, output: Path) -> None:
    kind, tool = renderer
    if kind == "rsvg":
        subprocess.run(
            [tool, "-w", str(size), "-h", str(size), str(svg_path), "-o", str(output)],
            check=True,
        )
        return
    subprocess.run(
        [
            tool,
            "--export-type=png",
            "--export-width",
            str(size),
            "--export-height",
            str(size),
            "--export-filename",
            str(output),
            str(svg_path),
        ],
        check=True,
    )


def generate_iconset(svg_path: Path, iconset_dir: Path) -> None:
    renderer = resolve_svg_renderer()
    sizes = [
        (16, "icon_16x16.png"),
        (32, "icon_16x16@2x.png"),
        (32, "icon_32x32.png"),
        (64, "icon_32x32@2x.png"),
        (128, "icon_128x128.png"),
        (256, "icon_128x128@2x.png"),
        (256, "icon_256x256.png"),
        (512, "icon_256x256@2x.png"),
        (512, "icon_512x512.png"),
        (1024, "icon_512x512@2x.png"),
    ]
    iconset_dir.mkdir(parents=True, exist_ok=True)
    for size, name in sizes:
        render_png(renderer, svg_path, size, iconset_dir / name)


def build_icns(svg_path: Path, output: Path) -> None:
    iconutil = shutil.which("iconutil")
    if not iconutil:
        raise SystemExit("未找到 iconutil，仅支持在 macOS 生成 icns")
    tmp_root = Path(tempfile.mkdtemp(prefix="iconset_"))
    iconset_dir = tmp_root / "app.iconset"
    generate_iconset(svg_path, iconset_dir)
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([iconutil, "-c", "icns", str(iconset_dir), "-o", str(output)], check=True)
    shutil.rmtree(tmp_root)


def build_ico(svg_path: Path, output: Path) -> None:
    magick = shutil.which("magick")
    convert = shutil.which("convert")
    tool = magick or convert
    if not tool:
        raise SystemExit("未找到 ImageMagick（magick/convert）")
    tmp_dir = Path(tempfile.mkdtemp(prefix="ico_"))
    sizes = [256, 128, 64, 48, 32, 16]
    try:
        renderer = resolve_svg_renderer()
        master_size = 256
        master_png = tmp_dir / f"icon_{master_size}x{master_size}.png"
        render_png(renderer, svg_path, master_size, master_png)
        for size in sizes:
            out_png = tmp_dir / f"icon_{size}x{size}.png"
            if size == master_size:
                master_png.replace(out_png)
                master_png = out_png
                continue
            resize_cmd = [
                tool,
                str(master_png),
                "-filter",
                "lanczos",
                "-define",
                "filter:blur=0.9",
                "-resize",
                f"{size}x{size}",
                "-unsharp",
                "0x0.5+0.5+0.008",
                str(out_png),
            ]
            subprocess.run(resize_cmd, check=True)
        output.parent.mkdir(parents=True, exist_ok=True)
        pngs = [str(tmp_dir / f"icon_{size}x{size}.png") for size in sizes]
        cmd = [
            tool,
            "-background",
            "none",
            *pngs,
            str(output),
        ]
        subprocess.run(cmd, check=True)
    finally:
        shutil.rmtree(tmp_dir)


def main() -> int:
    args = parse_args()
    base_dir = Path(__file__).resolve().parent.parent
    default_svg = base_dir / "resources" / "icons" / "app.svg"
    default_icns = base_dir / "resources" / "icons" / "app.icns"
    default_ico = base_dir / "resources" / "icons" / "app.ico"
    svg_path = Path(args.svg).expanduser().resolve() if args.svg else default_svg
    icns_path = Path(args.icns) if args.icns else None
    ico_path = Path(args.ico) if args.ico else None
    if icns_path is None and ico_path is None:
        icns_path = default_icns
        ico_path = default_ico
    if not svg_path.exists():
        raise SystemExit(f"未找到 svg 文件：{svg_path}")
    if icns_path:
        build_icns(svg_path, icns_path)
    if ico_path:
        build_ico(svg_path, ico_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
