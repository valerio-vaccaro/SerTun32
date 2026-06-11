"""Normalize compiler paths so artifacts do not depend on checkout location."""

from pathlib import Path

Import("env")

project_dir = Path(env.subst("$PROJECT_DIR")).resolve()
packages_dir = Path(env.subst("$PROJECT_PACKAGES_DIR")).resolve()

prefix_maps = (
    (project_dir, "/src"),
    (packages_dir, "/platformio/packages"),
)

flags = []
for source, replacement in prefix_maps:
    for option in ("file", "debug", "macro"):
        flags.append(f"-f{option}-prefix-map={source}={replacement}")

env.Append(CCFLAGS=flags)
