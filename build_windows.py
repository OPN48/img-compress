from pathlib import Path
import os
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
        encoding="utf-8",
        errors="replace",
        env=env,
    )
    stdout = process.stdout
    if stdout is not None:
        for line in stdout:
            print(line, end="")
    process.wait()
    if process.returncode != 0:
        raise SystemExit(process.returncode)


def main() -> None:
    root_dir = Path(__file__).resolve().parent
    spec_path = root_dir / "imgcompress.spec"
    cache_dir = root_dir / ".pyinstaller_cache"
    env = dict(os.environ)
    env["PYINSTALLER_CACHE_DIR"] = str(cache_dir)
    work_dir = Path(tempfile.mkdtemp(prefix="imgcompress_work_"))
    print("开始 PyInstaller 打包...")
    run_command(
        [sys.executable, "-m", "PyInstaller", "--workpath", str(work_dir), str(spec_path)],
        root_dir,
        env,
    )
    dist_exe = root_dir / "dist" / "Imgcompress.exe"
    if not dist_exe.exists():
        raise SystemExit(f"未找到可执行文件：{dist_exe}")
    print(f"已生成：{dist_exe}")


if __name__ == "__main__":
    main()
