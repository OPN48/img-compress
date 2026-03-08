from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import winreg


def run_command(command: list[str], cwd: Path) -> None:
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
        )
    except Exception:
        cmdline = " ".join(f'"{c}"' if (" " in c and not c.startswith('"')) else c for c in command)
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
            shell=True,
        )
    s = p.stdout
    if s is not None:
        for line in s:
            print(line, end="")
    p.wait()
    if p.returncode != 0:
        raise SystemExit(p.returncode)


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


def resolve_iscc() -> str:

    v = os.environ.get("INNOSETUP_ISCC")
    if v and Path(v).exists():
        return v
    if v:
        ve = v + ".exe"
        if Path(ve).exists():
            return ve
    iscc, _ = find_inno_bins()
    if iscc and Path(iscc).exists():
        return iscc
    for name in ["iscc", "ISCC"]:
        w = shutil.which(name)
        if w:
            return w


def resolve_inno_compiler() -> tuple[str, str]:
    try:
        iscc = resolve_iscc()
        return iscc, "iscc"
    except SystemExit:
        pass
    _, compil = find_inno_bins()
    if compil and Path(compil).exists():
        return compil, "compil32"
    for name in ["Compil32", "Compil32.exe"]:
        w = shutil.which(name)
        if w:
            return w, "compil32"
    raise SystemExit("未找到 Inno Setup 编译器，请安装 Inno Setup 或将 ISCC/Compil32 加入 PATH")


def read_reg_value(root: int, path: str, name: str, wow64: int) -> str | None:
    try:
        with winreg.OpenKey(root, path, 0, winreg.KEY_READ | wow64) as key:
            try:
                v, _ = winreg.QueryValueEx(key, name)
                return str(v)
            except OSError:
                return None
    except OSError:
        return None


def find_inno_install_dir() -> str | None:
    keys = [
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1"),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1"),
        (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1"),
        (winreg.HKEY_CURRENT_USER, r"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1"),
    ]
    names = ["InstallLocation", "InstallDir", "Inno Setup: App Path"]
    for root, path in keys:
        for name in names:
            v = read_reg_value(root, path, name, winreg.KEY_WOW64_64KEY)
            if v and Path(v).exists():
                return v
            v = read_reg_value(root, path, name, winreg.KEY_WOW64_32KEY)
            if v and Path(v).exists():
                return v
    return None


def find_inno_bins() -> tuple[str | None, str | None]:
    base = find_inno_install_dir()
    if not base:
        return None, None
    iscc_exe = Path(base) / "ISCC.exe"
    iscc_noext = Path(base) / "ISCC"
    compil_exe = Path(base) / "Compil32.exe"
    compil_noext = Path(base) / "Compil32"
    iscc = str(iscc_exe if iscc_exe.exists() else (iscc_noext if iscc_noext.exists() else "")) or None
    compil = str(compil_exe if compil_exe.exists() else (compil_noext if compil_noext.exists() else "")) or None
    return iscc, compil


def resolve_signtool() -> str | None:
    w = shutil.which("signtool")
    if w:
        return w
    candidates = [
        r"C:\Program Files (x86)\Windows Kits\10\bin\x64\signtool.exe",
        r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\signtool.exe",
        r"C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\vsdevcmd\core\signtool.exe",
    ]
    for c in candidates:
        if Path(c).exists():
            return c
    return None


def sign_file(signtool: str, file_path: Path, cert_pfx: str, cert_pwd: str, tsa: str | None) -> None:
    args = [signtool, "sign", "/fd", "sha256", "/tr", tsa or "http://timestamp.digicert.com", "/td", "sha256", "/f", cert_pfx, "/p", cert_pwd, str(file_path)]
    run_command(args, file_path.parent)


def main() -> None:
    script_dir = Path(__file__).resolve().parent
    native_dir = script_dir.parents[1]
    repo = native_dir.parent
    app_cfg = load_app_config(native_dir)
    app_name = read_app_value(app_cfg, "app_name", "Imgcompress")
    app_executable = read_app_value(app_cfg, "app_executable", "ImgcompressNative")
    iss = script_dir / "imgcompress.iss"
    if not iss.exists():
        raise SystemExit("未找到安装脚本 imgcompress.iss")
    run_command([sys.executable, str(native_dir / "build_windows.py")], repo)
    compiler, mode = resolve_inno_compiler()
    defs = [f"/DAppName={app_name}", f"/DAppExeName={app_executable}.exe"]
    if mode == "iscc":
        run_command([compiler, *defs, str(iss)], repo)
    else:
        run_command([compiler, "/cc", *defs, str(iss)], repo)
    out_dir = script_dir
    installer = None
    latest_time = 0.0
    for f in out_dir.glob("*.exe"):
        t = f.stat().st_mtime
        if t > latest_time:
            latest_time = t
            installer = f
    cert_pfx = os.environ.get("SIGN_CERT_PFX")
    cert_pwd = os.environ.get("SIGN_CERT_PWD")
    tsa = os.environ.get("SIGN_TSA")
    if cert_pfx and cert_pwd:
        signtool = resolve_signtool()
        if not signtool:
            raise SystemExit("请求签名但未找到 signtool，请安装 Windows SDK 或将 signtool 加入 PATH")
        app_exe = native_dir / "dist" / f"{app_executable}.exe"
        if app_exe.exists():
            sign_file(signtool, app_exe, cert_pfx, cert_pwd, tsa)
        if installer and installer.exists():
            sign_file(signtool, installer, cert_pfx, cert_pwd, tsa)
        print("已完成代码签名")
    print("安装程序已生成")


if __name__ == "__main__":
    main()
