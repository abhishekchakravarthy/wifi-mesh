#!/usr/bin/env python3
import argparse
import os
import re
import shlex
import subprocess
import sys
import time
import glob as _glob
from pathlib import Path
from typing import List, Optional, Tuple


REPO_ROOT = Path(__file__).resolve().parent
ANDROID_DIR = REPO_ROOT / "android"
ESP_A_DIR = REPO_ROOT  # Root project (ESP32 A - coordinator)
ESP_B_DIR = REPO_ROOT / "esp32_b_project"  # ESP32 B - client


def run_cmd(cmd: List[str], cwd: Optional[Path] = None, check: bool = True) -> Tuple[int, str, str]:
    proc = subprocess.Popen(cmd, cwd=str(cwd) if cwd else None, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = proc.communicate()
    if check and proc.returncode != 0:
        raise subprocess.CalledProcessError(proc.returncode, cmd, output=out, stderr=err)
    return proc.returncode, out, err


def which(executable: str) -> Optional[str]:
    from shutil import which as _which
    return _which(executable)


def find_serial_ports() -> List[str]:
    candidates = []
    for pattern in ("/dev/ttyACM", "/dev/ttyUSB"):
        for i in range(0, 16):
            p = f"{pattern}{i}"
            if os.path.exists(p):
                candidates.append(p)
    # sort deterministically
    return sorted(set(candidates))


def detect_android_devices() -> List[str]:
    if not which("adb"):
        return []
    try:
        _, out, _ = run_cmd(["adb", "devices", "-l"], check=False)
    except Exception:
        return []
    serials = []
    for line in out.splitlines():
        # Lines look like: "<serial>       device ..." (with spaces, not tabs)
        if "device" in line and not line.lower().startswith("list of devices") and line.strip():
            # Split on whitespace and take the first part (device serial)
            parts = line.split()
            if len(parts) >= 2 and parts[1] == "device":
                serial = parts[0].strip()
                if serial:
                    serials.append(serial)
    return serials


def read_pio_env(project_dir: Path) -> Optional[str]:
    ini_path = project_dir / "platformio.ini"
    if not ini_path.exists():
        return None
    content = ini_path.read_text(errors="ignore")
    # Find first [env:...] section header
    m = re.search(r"^\s*\[env:([^\]]+)\]", content, flags=re.MULTILINE)
    if m:
        return m.group(1)
    return None


def build_and_flash(project_dir: Path, upload_port: str, env_name: Optional[str]) -> None:
    if not which("pio"):
        raise RuntimeError("PlatformIO CLI 'pio' not found. Install with: pip install platformio")
    env_args = (["-e", env_name] if env_name else [])
    print(f"\n=== Building {project_dir} (env: {env_name or 'auto'}) ===")
    run_cmd(["pio", "run", "-d", str(project_dir)] + env_args)
    print(f"\n=== Flashing to {upload_port} ===")
    ensure_serial_port_free(upload_port)
    run_cmd(["pio", "run", "-d", str(project_dir), "--target", "upload", "--upload-port", upload_port] + env_args)


def build_android(sdk_path: Path, variant: str) -> Path:
    gradlew = "gradlew.bat" if os.name == "nt" else "gradlew"
    gradlew_path = ANDROID_DIR / gradlew
    if not gradlew_path.exists():
        raise RuntimeError(f"Gradle wrapper not found at {gradlew_path}")
    # Ensure local.properties has a valid sdk.dir
    if not sdk_path or not sdk_path.exists():
        raise RuntimeError("Android SDK not found. Set ANDROID_SDK_ROOT/ANDROID_HOME, pass --android-sdk, or install to ~/Android/Sdk.")
    ensure_local_properties_sdk(sdk_path)
    task = "assembleDebug" if variant.lower() == "debug" else "assembleRelease"
    print(f"\n=== Building Android app ({variant}) ===")
    # Ensure wrapper is executable on POSIX
    if os.name != "nt":
        try:
            os.chmod(gradlew_path, os.stat(gradlew_path).st_mode | 0o111)
        except Exception:
            pass
    run_cmd([str(gradlew_path), task], cwd=ANDROID_DIR)
    # Determine APK output path
    if variant.lower() == "debug":
        apk = ANDROID_DIR / "app" / "build" / "outputs" / "apk" / "debug" / "app-debug.apk"
    else:
        apk = ANDROID_DIR / "app" / "build" / "outputs" / "apk" / "release" / "app-release.apk"
    if not apk.exists():
        # Try universal fallback search
        candidates = list((ANDROID_DIR / "app" / "build" / "outputs" / "apk").rglob("*.apk"))
        if not candidates:
            raise RuntimeError("APK not found after build.")
        # Prefer matching variant
        for c in candidates:
            if variant.lower() in c.name.lower():
                apk = c
                break
        else:
            apk = max(candidates, key=lambda p: p.stat().st_mtime)
    print(f"APK: {apk}")
    return apk


def parse_android_package_name() -> Optional[str]:
    manifest = ANDROID_DIR / "app" / "src" / "main" / "AndroidManifest.xml"
    if not manifest.exists():
        return None
    try:
        import xml.etree.ElementTree as ET
        root = ET.parse(manifest).getroot()
        return root.attrib.get("package")
    except Exception:
        return None


def install_apk_to_devices(apk_path: Path, device_serials: List[str]) -> None:
    if not which("adb"):
        raise RuntimeError("'adb' not found. Please install Android platform-tools and add to PATH.")
    for serial in device_serials:
        print(f"\n=== Installing APK to device {serial} ===")
        # -r: replace existing, -d: allow version downgrade
        run_cmd(["adb", "-s", serial, "install", "-r", "-d", str(apk_path)], check=False)


def launch_android_app(device_serials: List[str], package_name: Optional[str]) -> None:
    if not package_name:
        return
    for serial in device_serials:
        print(f"Launching app {package_name} on {serial}")
        run_cmd(["adb", "-s", serial, "shell", "monkey", "-p", package_name, "-c", "android.intent.category.LAUNCHER", "1"], check=False)


def default_port_mapping(ports: List[str]) -> Tuple[Optional[str], Optional[str]]:
    if not ports:
        return None, None
    if len(ports) == 1:
        return ports[0], None
    # Heuristic: lower-numbered port -> ESP32 A, next -> ESP32 B
    return ports[0], ports[1]


def locate_android_sdk() -> Optional[Path]:
    env_paths = [
        os.environ.get("ANDROID_SDK_ROOT"),
        os.environ.get("ANDROID_HOME"),
        str(Path.home() / "android-sdk"),  # Updated to match actual location
        str(Path.home() / "Android" / "Sdk"),  # Keep original as fallback
    ]
    for p in env_paths:
        if p and os.path.isdir(p):
            return Path(p)
    return None


def ensure_local_properties_sdk(sdk_path: Path) -> None:
    lp = ANDROID_DIR / "local.properties"
    desired = f"sdk.dir={sdk_path}"
    lines: List[str] = []
    if lp.exists():
        try:
            lines = lp.read_text().splitlines()
        except Exception:
            lines = []
    updated = False
    new_lines: List[str] = []
    for line in lines:
        if line.strip().startswith("sdk.dir="):
            if line.strip() != desired:
                new_lines.append(desired)
                updated = True
            else:
                new_lines.append(line)
            # skip appending any duplicates of sdk.dir
        else:
            new_lines.append(line)
    if not any(l.strip().startswith("sdk.dir=") for l in new_lines):
        new_lines.append(desired)
        updated = True
    if updated or not lp.exists():
        lp.write_text("\n".join(new_lines) + "\n")


def pids_using_file(path: str) -> List[int]:
    pids: List[int] = []
    # Try lsof first
    if which("lsof"):
        try:
            code, out, _ = run_cmd(["lsof", "-t", "--", path], check=False)
            if code == 0 and out.strip():
                for line in out.split():
                    try:
                        pids.append(int(line.strip()))
                    except ValueError:
                        pass
        except Exception:
            pass
    # Fallback: fuser without killing, parse PIDs
    if not pids and which("fuser"):
        try:
            code, out, _ = run_cmd(["fuser", path], check=False)
            if code == 0 and out.strip():
                for token in out.split():
                    token = token.strip()
                    # fuser may append postfixes like '1234c'
                    token = re.sub(r"[^0-9]", "", token)
                    if token:
                        try:
                            pids.append(int(token))
                        except ValueError:
                            pass
        except Exception:
            pass
    return sorted(set(pids))


def kill_pids(pids: List[int], force: bool = False) -> None:
    if not pids:
        return
    sig = "-KILL" if force else "-TERM"
    try:
        run_cmd(["kill", sig] + [str(pid) for pid in pids], check=False)
    except Exception:
        pass


def ensure_serial_port_free(port: str) -> None:
    print(f"Checking if {port} is in use...")
    pids = pids_using_file(port)
    if not pids:
        print("Port is free.")
        return
    print(f"Port is busy by PIDs: {pids}. Attempting to free it...")
    # Try targeted kill with TERM first
    kill_pids(pids, force=False)
    time.sleep(0.5)
    remain = pids_using_file(port)
    if remain:
        print(f"Processes still holding {port}: {remain}. Escalating to KILL...")
        kill_pids(remain, force=True)
        time.sleep(0.5)
    # As last resort, use fuser -k on the path
    if which("fuser") and pids_using_file(port):
        run_cmd(["fuser", "-k", port], check=False)
        time.sleep(0.3)
    # Final check
    if pids_using_file(port):
        print(f"Warning: {port} still appears busy. Flash may fail if another process is holding the port.")


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Deploy WiFi-Mesh: flash ESP32 A/B and build/install Android app")
    parser.add_argument("--port-a", help="Serial port for ESP32 A (coordinator), e.g. /dev/ttyACM0")
    parser.add_argument("--port-b", help="Serial port for ESP32 B (client), e.g. /dev/ttyACM1")
    parser.add_argument("--env-a", help="PlatformIO env name for ESP32 A (defaults to first env in platformio.ini)")
    parser.add_argument("--env-b", help="PlatformIO env name for ESP32 B (defaults to first env in platformio.ini)")
    parser.add_argument("--android-sdk", help="Path to Android SDK root (overrides env and defaults)")
    parser.add_argument("--android-variant", default="debug", choices=["debug", "release"], help="Android build variant")
    parser.add_argument("--use-glob-install", action="store_true", help="Install all APKs matching --apk-glob to all connected devices")
    parser.add_argument("--apk-glob", default="android/app/build/outputs/apk/debug/*.apk", help="APK glob used with --use-glob-install")
    parser.add_argument("--skip-esp", action="store_true", help="Skip ESP32 build/flash")
    parser.add_argument("--skip-android", action="store_true", help="Skip Android build/install")
    parser.add_argument("--no-launch", action="store_true", help="Do not auto-launch app after install")
    args = parser.parse_args(argv)

    print("=== Deploy WiFi-Mesh ===")
    
    # Set Android SDK environment variables if not already set
    if not os.environ.get("ANDROID_SDK_ROOT") and not os.environ.get("ANDROID_HOME"):
        sdk_path = str(Path.home() / "android-sdk")
        if os.path.isdir(sdk_path):
            os.environ["ANDROID_SDK_ROOT"] = sdk_path
            os.environ["ANDROID_HOME"] = sdk_path
            print(f"Auto-set Android SDK environment variables to: {sdk_path}")

    # Detect ports if not provided
    ports = find_serial_ports()
    port_a = args.port_a
    port_b = args.port_b
    if not port_a or (not port_b and not args.skip_esp):
        da, db = default_port_mapping(ports)
        port_a = port_a or da
        port_b = port_b or db
    print(f"Detected serial ports: {ports}")
    print(f"Using port A: {port_a or '-'} | port B: {port_b or '-'}")

    # Resolve envs
    env_a = args.env_a or read_pio_env(ESP_A_DIR)
    env_b = args.env_b or read_pio_env(ESP_B_DIR)
    print(f"PlatformIO env A: {env_a or 'auto'} | env B: {env_b or 'auto'}")

    # ESP32 build/flash
    if not args.skip_esp:
        if not port_a:
            print("Warning: No serial port found for ESP32 A. Skipping ESP32 flashing.")
        else:
            build_and_flash(ESP_A_DIR, port_a, env_a)
        if port_b:
            build_and_flash(ESP_B_DIR, port_b, env_b)
        else:
            print("Warning: No serial port found for ESP32 B. Skipping ESP32 B flashing.")

    # Android build/install
    if not args.skip_android:
        # Resolve Android SDK
        sdk = Path(args.android_sdk).expanduser().resolve() if args.android_sdk else locate_android_sdk()
        if not sdk or not sdk.exists():
            raise RuntimeError("Android SDK not found. Provide --android-sdk or set ANDROID_SDK_ROOT/ANDROID_HOME.")
        devices = detect_android_devices()
        print(f"Connected Android devices: {devices if devices else 'none'}")
        if not devices:
            print("Warning: No Android devices found via 'adb devices'. Skipping install.")
        else:
            if args.use_glob_install:
                # Install all APKs matching glob to every connected device (user-style loop)
                apk_pattern = (REPO_ROOT / args.apk_glob).as_posix()
                matches = sorted(_glob.glob(apk_pattern))
                if not matches:
                    # Ensure build so that APK exists
                    _ = build_android(sdk, args.android_variant)
                    matches = sorted(_glob.glob(apk_pattern))
                for serial in devices:
                    print(f"\n=== Installing matching APKs to {serial} ===")
                    for apk_file in matches:
                        print(f"Installing {apk_file}")
                        run_cmd(["adb", "-s", serial, "install", "-r", "-d", apk_file], check=False)
                if not args.no_launch:
                    pkg = parse_android_package_name()
                    launch_android_app(devices, pkg)
            else:
                # Normal flow: build specific variant and install that APK
                apk = build_android(sdk, args.android_variant)
                install_apk_to_devices(apk, devices)
                if not args.no_launch:
                    pkg = parse_android_package_name()
                    launch_android_app(devices, pkg)

    print("\n✅ Deployment finished.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as e:
        cmd_str = " ".join(shlex.quote(c) for c in (e.cmd if isinstance(e.cmd, list) else [str(e.cmd)]))
        sys.stderr.write(f"\n❌ Command failed ({e.returncode}): {cmd_str}\n")
        if e.output:
            sys.stderr.write(e.output + "\n")
        if e.stderr:
            sys.stderr.write(e.stderr + "\n")
        sys.exit(e.returncode)
    except Exception as e:
        sys.stderr.write(f"\n❌ {e}\n")
        sys.exit(1)


