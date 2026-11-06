#!/usr/bin/env python3
"""Check publishable files, documentation, installation tools and firmware policies.

Requires Git and Python's standard library. Never builds firmware, flashes a
device, stages files, deletes local data or edits Git metadata.
"""

import argparse
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import unittest
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
LOCAL_PARTS = {".pio", ".git", ".local-tools", ".platformio", ".venv", "venv",
               ".vscode", ".idea", "node_modules", "__pycache__", ".pytest_cache", ".ruff_cache", "logs"}
LOCAL_NAMES = {".env", ".mcp.json", "credentials.json", "secrets.ini", "dash_key.pem", "dash_cert.pem"}
SECRET_PATTERNS = (
    re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    re.compile(r"\bgh[pousr]_[A-Za-z0-9]{30,}\b"),
    re.compile(r"\bgithub_pat_[A-Za-z0-9_]{40,}\b"),
)
MARKDOWN_LINK = re.compile(r"!?\[[^\]\n]*\]\(([^\s)]+)(?:\s+\"[^\"]*\")?\)")
HTML_ASSET = re.compile(r"\b(?:src|href)=[\"']([^\"']+)[\"']")


def publishable_files(root=ROOT):
    """Include existing tracked and nonignored new files; ignore pending deletions."""
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        check=True, stdout=subprocess.PIPE,
    )
    return sorted({name.decode("utf-8") for name in result.stdout.split(b"\0")
                   if name and (root / name.decode("utf-8")).is_file()})


def is_local_artifact(name):
    path = PurePosixPath(name)
    return (bool(set(path.parts) & LOCAL_PARTS)
            or path.name in LOCAL_NAMES
            or (path.name.startswith(".env.") and path.name != ".env.example")
            or path.suffix.lower() in {".log", ".dmp", ".bak", ".orig", ".pyc", ".elf",
                                       ".tmp", ".swp", ".swo", ".exe", ".o", ".obj"}
            or name.startswith(("meshcore/tmp/", "meshcore/sdcard/", "meshcore/tools/tiles/"))
            or name.startswith("meshcore/tools/emoji_atlas_") and path.suffix == ".bin")


def is_owned_text(name):
    path = PurePosixPath(name)
    if path.suffix.lower() not in {".py", ".cpp", ".c", ".h", ".md", ".ini", ".json", ".yml"}:
        return False
    return (len(path.parts) == 1 or name.startswith((
        "tools/", "docs/", ".github/", "selector/src/",
        "meshcore/examples/crowpanel_p4_lvgl_chat/", "meshcore/variants/crowpanel_dhe04005d/",
        "meshtastic/src/graphics/mcui/", "meshtastic/src/mqtt/", "meshtastic/test/host/",
        "meshtastic/variants/esp32p4/crowpanel_dhe04005d/",
    )))


def documentation_files(root=ROOT):
    paths = [root / name for name in ("README.md", "CONTRIBUTING.md", "SECURITY.md", "CHANGELOG.md",
             "meshcore/README.md", "meshcore/CONTRIBUTING.md", "meshcore/SECURITY.md", "meshcore/RELEASE.md",
             "meshcore/test/README.md", "meshcore/docs/crowpanel.md", "meshcore/docs/crash_history.md",
             "meshcore/variants/crowpanel_dhe04005d/README.md",
             "meshtastic/README.md", "meshtastic/DEVELOPMENT.md", "meshtastic/test/host/README.md",
             "meshtastic/variants/esp32p4/crowpanel_dhe04005d/README.md")]
    paths.extend((root / "docs").glob("*.md"))
    return sorted(set(paths))


def local_link_errors(document, root=ROOT):
    text = document.read_text(encoding="utf-8")
    # Examples in fenced blocks are not navigation links.
    text = re.sub(r"```.*?```", "", text, flags=re.DOTALL)
    targets = MARKDOWN_LINK.findall(text) + HTML_ASSET.findall(text)
    errors = []
    for target in targets:
        url = urlsplit(target.strip("<>"))
        if url.scheme or url.netloc or not url.path:
            continue
        relative = unquote(url.path)
        if relative.startswith("/"):
            errors.append(f"Root-absolute documentation link: {target}")
            continue
        path = Path(os.path.abspath(document.parent / relative))
        if not path.resolve().is_relative_to(root.resolve()):
            errors.append(f"Documentation link escapes repository: {target}")
            continue
        current = root.resolve()
        for part in path.relative_to(current).parts:
            if not current.is_dir() or part not in {child.name for child in current.iterdir()}:
                errors.append(f"Missing or incorrectly capitalized link: {target}")
                break
            current /= part
    return errors


def hygiene_errors(root=ROOT):
    errors = []
    for name in publishable_files(root):
        path = root / name
        if is_local_artifact(name):
            errors.append(f"Local/private artifact would be published: {name}")
        if path.stat().st_size >= 100 * 1024 * 1024:
            errors.append(f"File reaches GitHub's 100 MiB limit: {name}")
        if is_owned_text(name):
            text = path.read_text(encoding="utf-8")
            if any(pattern.search(text) for pattern in SECRET_PATTERNS):
                errors.append(f"Possible private key/token in {name}; inspect locally")
    for document in documentation_files(root):
        if not document.is_file():
            errors.append(f"Missing documentation: {document.relative_to(root)}")
            continue
        errors.extend(f"{document.relative_to(root)}: {error}" for error in local_link_errors(document, root))
    return errors


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", help="C++17 compiler passed to the MeshCore check runner")
    parser.add_argument("--python-only", action="store_true", help="Explicitly skip C++ policy compilation")
    args = parser.parse_args(argv)
    try:
        errors = hygiene_errors()
    except (OSError, UnicodeError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Repository inspection failed: {error}", file=sys.stderr)
        return 1
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print("Publishable-file and local documentation checks passed.", flush=True)
    suite = unittest.defaultTestLoader.discover(str(ROOT / "test"), pattern="test_*.py")
    if not suite.countTestCases():
        print("No repository tooling tests found.", file=sys.stderr)
        return 1
    if not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful():
        return 1
    command = [sys.executable, str(ROOT / "meshcore/tools/check_crowpanel.py")]
    if args.compiler:
        command.extend(("--compiler", args.compiler))
    if args.python_only:
        command.append("--python-only")
    result = subprocess.run(command, cwd=ROOT / "meshcore", check=False).returncode
    if result or args.python_only:
        return result
    command = [sys.executable, str(ROOT / "meshtastic/test/host/check_policies.py")]
    if args.compiler:
        command.extend(("--compiler", args.compiler))
    return subprocess.run(command, cwd=ROOT, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
