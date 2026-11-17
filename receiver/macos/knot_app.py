"""KnotReceiver.app entry point (macOS).

Double-click: choose the Obsidian vault on first run, install a per-user LaunchAgent that runs
this same app with --agent at login and restarts it if it exits, then show the status.
--agent: run the BLE receiver (what launchd starts).
The app embeds Python (PyInstaller, onedir) so the bundle itself holds the Bluetooth permission.
"""
from __future__ import annotations

import json
import os
import plistlib
import subprocess
import sys
from pathlib import Path

import knot_receiver

LABEL = "io.github.knot-notes.receiver"
PLIST = Path.home() / "Library" / "LaunchAgents" / f"{LABEL}.plist"
LOGDIR = Path.home() / "Library" / "Logs" / "KnotReceiver"
DOMAIN = f"gui/{os.getuid()}"


def osascript(script: str) -> str:
    result = subprocess.run(["osascript", "-e", script], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else ""


def dialog(message: str, buttons=("好",), default=None) -> str:
    quoted = ", ".join(json.dumps(b, ensure_ascii=False) for b in buttons)
    default = json.dumps(default or buttons[-1], ensure_ascii=False)
    out = osascript(f'button returned of (display dialog {json.dumps(message, ensure_ascii=False)} '
                    f'with title "Knot 结绳" buttons {{{quoted}}} default button {default})')
    return out


def choose_vault() -> str | None:
    path = osascript('POSIX path of (choose folder with prompt "选择你的 Obsidian 库（包含 .obsidian 的文件夹）")')
    return path.rstrip("/") or None


def save_config(config: dict):
    path = knot_receiver.config_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def launchctl(*args) -> int:
    return subprocess.run(["launchctl", *args], capture_output=True).returncode


def agent_loaded() -> bool:
    return launchctl("print", f"{DOMAIN}/{LABEL}") == 0


def install_agent():
    LOGDIR.mkdir(parents=True, exist_ok=True)
    PLIST.parent.mkdir(parents=True, exist_ok=True)
    with PLIST.open("wb") as handle:
        plistlib.dump({
            "Label": LABEL,
            "ProgramArguments": [sys.executable, "--agent"],
            "RunAtLoad": True,
            "KeepAlive": True,          # restart after any exit, including crashes
            "ThrottleInterval": 10,
            "ProcessType": "Background",
            "StandardOutPath": str(LOGDIR / "receiver.log"),
            "StandardErrorPath": str(LOGDIR / "receiver.log"),
        }, handle)
    launchctl("bootout", f"{DOMAIN}/{LABEL}")
    launchctl("bootstrap", DOMAIN, str(PLIST))
    launchctl("enable", f"{DOMAIN}/{LABEL}")


def uninstall_agent():
    launchctl("bootout", f"{DOMAIN}/{LABEL}")
    PLIST.unlink(missing_ok=True)


def setup():
    if sys.executable.startswith("/Volumes/"):
        dialog("请先把 KnotReceiver 拖进「应用程序」文件夹，再从那里打开。")
        return
    config = knot_receiver.load_config()
    if not config.get("vault") or not Path(config["vault"]).is_dir():
        vault = choose_vault()
        if not vault:
            return
        if not (Path(vault) / ".obsidian").is_dir() and dialog(
                "这个文件夹里没有 .obsidian，看起来不是 Obsidian 库。仍然使用？", ("取消", "使用")) != "使用":
            return
        config["vault"] = vault
        save_config(config)
    if agent_loaded():
        choice = dialog(f"结绳接收端正在后台运行。\n\n库：{config['vault']}\n日志：{LOGDIR / 'receiver.log'}",
                        ("停止并移除", "重新选择库", "好"))
        if choice == "停止并移除":
            uninstall_agent()
            dialog("已停止，并取消登录自启。再次打开本 App 即可重新启用。")
        elif choice == "重新选择库":
            vault = choose_vault()
            if vault:
                config["vault"] = vault
                save_config(config)
                install_agent()
        return
    install_agent()
    dialog("已在后台运行，并会在登录时自动启动、异常退出后自动重启。\n\n"
           "首次连接：系统会询问 KnotReceiver 能否使用蓝牙，请允许；设备屏幕显示 Pairing PIN 时，"
           "在 macOS 弹窗里输入那 6 位数字。")


def main():
    if "--agent" in sys.argv:
        sys.argv = [sys.argv[0]] + [a for a in sys.argv[1:] if a != "--agent"]
        knot_receiver.main()
    else:
        setup()


if __name__ == "__main__":
    main()
