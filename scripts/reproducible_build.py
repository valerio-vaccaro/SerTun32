"""Normalize compiler paths so artifacts do not depend on checkout location."""

from pathlib import Path
import os
import subprocess

Import("env")

project_dir = Path(env.subst("$PROJECT_DIR")).resolve()
packages_dir = Path(env.subst("$PROJECT_PACKAGES_DIR")).resolve()

version = os.environ.get("SERTUN_VERSION")
if not version:
    tags = subprocess.run(
        ["git", "-C", str(project_dir), "tag", "--sort=-version:refname", "--points-at", "HEAD"],
        capture_output=True, text=True, check=False,
    )
    version = tags.stdout.splitlines()[0] if tags.returncode == 0 and tags.stdout.strip() else "dev"
env.Append(CPPDEFINES=[("FIRMWARE_VERSION", f'\\"{version}\\"')])

prefix_maps = (
    (project_dir, "/src"),
    (packages_dir, "/platformio/packages"),
)

flags = []
for source, replacement in prefix_maps:
    for option in ("file", "debug", "macro"):
        flags.append(f"-f{option}-prefix-map={source}={replacement}")

env.Append(CCFLAGS=flags)
