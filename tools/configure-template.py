#!/usr/bin/env python3
"""Configure the LSPosed universal template for a new module.

Example:
  ./tools/configure-template.py \
    --package com.yourname.yourmodule \
    --name "Your Module" \
    --target com.example.target \
    --author "YourName"
"""

from __future__ import annotations

import argparse
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PACKAGE = "com.template.lsposed"
DEFAULT_JNI = DEFAULT_PACKAGE.replace(".", "_")
DEFAULT_NATIVE_LIB = "template_native"
SKIP_DIRS = {".git", ".gradle", ".idea", ".cxx", "build", ".externalNativeBuild"}
SKIP_SUFFIXES = {".jar", ".png", ".jpg", ".jpeg", ".webp", ".gif", ".so", ".a", ".zip", ".apk", ".xapk"}
PACKAGE_RE = re.compile(r"^[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z][a-zA-Z0-9_]*)+$")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", required=True, help="New module package/applicationId, e.g. com.acme.mod")
    parser.add_argument("--name", required=True, help="Human-readable module/app name")
    parser.add_argument("--target", required=True, action="append", help="Target package. Repeat or comma-separate multiple packages")
    parser.add_argument("--author", default="YourName", help="Author for module.prop")
    parser.add_argument("--old-package", default=DEFAULT_PACKAGE, help="Current package to replace")
    parser.add_argument("--native-lib", default=None,
                        help="Optional new native library short name (e.g. 'audio_util') replacing 'template_native'.")
    return parser.parse_args()


def normalized_targets(raw: list[str]) -> list[str]:
    out: list[str] = []
    for item in raw:
        for value in item.split(","):
            value = value.strip()
            if value and value not in out:
                out.append(value)
    if not out:
        raise SystemExit("At least one --target package is required")
    for value in out:
        if not PACKAGE_RE.match(value):
            raise SystemExit(f"Invalid target package: {value}")
    return out


def package_path(package: str) -> Path:
    return ROOT / "app" / "src" / "main" / "java" / Path(package.replace(".", "/"))


def move_package_dir(old_package: str, new_package: str) -> None:
    old_path = package_path(old_package)
    new_path = package_path(new_package)
    if old_path == new_path or not old_path.exists():
        return
    new_path.parent.mkdir(parents=True, exist_ok=True)
    if new_path.exists():
        raise SystemExit(f"Destination package path already exists: {new_path}")
    shutil.move(str(old_path), str(new_path))

    # Clean empty parent directories left behind under java/.
    java_root = ROOT / "app" / "src" / "main" / "java"
    parent = old_path.parent
    while parent != java_root and parent.exists():
        try:
            parent.rmdir()
        except OSError:
            break
        parent = parent.parent


def should_skip(path: Path) -> bool:
    rel_parts = path.relative_to(ROOT).parts
    if any(part in SKIP_DIRS for part in rel_parts):
        return True
    if path.suffix.lower() in SKIP_SUFFIXES:
        return True
    return False


def replace_in_text_files(old_package: str, new_package: str, module_name: str, author: str) -> None:
    old_jni = old_package.replace(".", "_")
    new_jni = new_package.replace(".", "_")
    old_slash = old_package.replace(".", "/")
    new_slash = new_package.replace(".", "/")
    replacements = {
        old_package: new_package,
        old_jni: new_jni,
        old_slash: new_slash,
        "LSPosed Universal Template": module_name,
        "Universal Module": module_name,
        "YourName": author,
    }
    for path in ROOT.rglob("*"):
        if not path.is_file() or should_skip(path):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        new_text = text
        for old, new in replacements.items():
            new_text = new_text.replace(old, new)
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")


def rewrite_scope_files(targets: list[str]) -> None:
    scope = "\n".join(targets) + "\n"
    (ROOT / "app" / "src" / "main" / "resources" / "META-INF" / "xposed" / "scope.list").write_text(scope, encoding="utf-8")

    items = "\n".join(f"        <item>{target}</item>" for target in targets)
    arrays = f'''<?xml version="1.0" encoding="utf-8"?>\n<resources>\n    <string-array name="xposed_scope">\n{items}\n    </string-array>\n</resources>\n'''
    (ROOT / "app" / "src" / "main" / "res" / "values" / "arrays.xml").write_text(arrays, encoding="utf-8")


NATIVE_LIB_RE = re.compile(r"^[a-z][a-z0-9_]*$")


def rewrite_native_lib(old_name: str, new_name: str) -> None:
    if not new_name or new_name == old_name:
        return
    if not NATIVE_LIB_RE.match(new_name):
        raise SystemExit(f"Invalid --native-lib value: {new_name} (use lowercase/digits/underscores)")

    # CMakeLists: add_library(<name> SHARED ...) and project(<name> ...)
    cmake_path = ROOT / "app" / "src" / "main" / "cpp" / "CMakeLists.txt"
    if cmake_path.exists():
        text = cmake_path.read_text(encoding="utf-8")
        text = re.sub(rf"\b{re.escape(old_name)}\b", new_name, text)
        cmake_path.write_text(text, encoding="utf-8")

    # Java usages: System.loadLibrary and the constant in TemplateConfig.
    java_root = ROOT / "app" / "src" / "main" / "java"
    for path in java_root.rglob("*.java"):
        text = path.read_text(encoding="utf-8")
        updated = text
        updated = updated.replace(f'System.loadLibrary("{old_name}")',
                                  f'System.loadLibrary("{new_name}")')
        updated = re.sub(
            r'(public static final String NATIVE_LIBRARY_NAME\s*=\s*")[^"]+(";)',
            rf'\g<1>{new_name}\g<2>', updated)
        if updated != text:
            path.write_text(updated, encoding="utf-8")

    # ProGuard and documentation references to lib<old>.so.
    proguard_path = ROOT / "app" / "proguard-rules.pro"
    if proguard_path.exists():
        text = proguard_path.read_text(encoding="utf-8")
        text = text.replace(f"lib{old_name}.so", f"lib{new_name}.so")
        proguard_path.write_text(text, encoding="utf-8")

    for path in ROOT.rglob("*"):
        if not path.is_file() or should_skip(path):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        updated = text.replace(f"lib{old_name}.so", f"lib{new_name}.so")
        if updated != text:
            path.write_text(updated, encoding="utf-8")


def rewrite_template_config(targets: list[str], module_name: str) -> None:
    config_path = ROOT / "app" / "src" / "main" / "java"
    matches = list(config_path.rglob("TemplateConfig.java"))
    if not matches:
        return
    path = matches[0]
    text = path.read_text(encoding="utf-8")
    target_block = ",\n".join(f'            "{target}"' for target in targets)
    text = re.sub(
        r"public static final String\[] TARGET_PACKAGES = \{.*?\n    \};",
        "public static final String[] TARGET_PACKAGES = {\n" + target_block + "\n    };",
        text,
        flags=re.S,
    )
    text = re.sub(r'public static final String MENU_TITLE = ".*?";',
                  f'public static final String MENU_TITLE = "{module_name}";', text)
    path.write_text(text, encoding="utf-8")


def main() -> None:
    args = parse_args()
    if not PACKAGE_RE.match(args.package):
        raise SystemExit(f"Invalid package name: {args.package}")
    targets = normalized_targets(args.target)

    move_package_dir(args.old_package, args.package)
    replace_in_text_files(args.old_package, args.package, args.name, args.author)
    rewrite_scope_files(targets)
    rewrite_template_config(targets, args.name)
    if args.native_lib:
        rewrite_native_lib(DEFAULT_NATIVE_LIB, args.native_lib)

    print("Configured template")
    print(f"  package:    {args.package}")
    print(f"  name:       {args.name}")
    print(f"  targets:    {', '.join(targets)}")
    print(f"  native lib: lib{(args.native_lib or DEFAULT_NATIVE_LIB)}.so")


if __name__ == "__main__":
    main()
