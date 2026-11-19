"""
Pre-build script: inject firmware version from git when available.
When the project is extracted from a ZIP archive (without .git), use a
stable local version string so PlatformIO can still build it.
"""
Import("env")  # noqa: F821 — PlatformIO SCons global

import os
import subprocess

cwd = env["PROJECT_DIR"]
version = "1.2"

try:
    tag = subprocess.check_output(
        ["git", "describe", "--tags", "--exact-match"],
        stderr=subprocess.DEVNULL, cwd=cwd,
    ).decode().strip().lstrip("v")
    if tag:
        version = tag
    else:
        raise RuntimeError("empty git tag")
except Exception:
    try:
        sha = subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL, cwd=cwd,
        ).decode().strip()
        if sha:
            dirty = subprocess.call(
                ["git", "diff", "--quiet", "--", ":(exclude)firmware/sdkconfig.*"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=cwd,
            ) != 0
            version = sha + ("-dirty" if dirty else "")
    except Exception:
        # ZIP extraction has no Git metadata; this is expected.
        version = "1.2"

print(f"Firmware version: {version}")

version_h = os.path.join(env["PROJECT_DIR"], "src", "version.h")
content = f'#pragma once\\n#define DORKY_FIRMWARE_VERSION "{version}"\\n'

try:
    with open(version_h, encoding="utf-8") as f:
        existing = f.read()
except FileNotFoundError:
    existing = ""

if existing != content:
    with open(version_h, "w", encoding="utf-8") as f:
        f.write(content)
