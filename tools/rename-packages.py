#!/usr/bin/env python3
"""Moves the code from the upstream packages into this fork's packages.

This fork lives in the package io.github.sendablemetatype.webrtc, upstream
in dev.onvoid.webrtc. Run this script from the repository root after every
merge from upstream: it moves the files that arrived under the upstream
package directories, rewrites the package, module and JNI symbol names in
every text file, and leaves everything that is already renamed alone.
"""

import os
import re
import subprocess
import sys

OLD_PACKAGE = "dev.onvoid.webrtc"
NEW_PACKAGE = "io.github.sendablemetatype.webrtc"

OLD_MODULE = "webrtc.java"
NEW_MODULE = "io.github.sendablemetatype.webrtc"

# In the order they must be applied: the path form first, so that the dotted
# form does not match inside it.
REPLACEMENTS = [
    (OLD_PACKAGE.replace(".", "/"), NEW_PACKAGE.replace(".", "/")),
    (OLD_PACKAGE.replace(".", "_"), NEW_PACKAGE.replace(".", "_")),
    (OLD_PACKAGE, NEW_PACKAGE),
]

MODULE_PATTERN = re.compile(r"(?<![\w.])" + re.escape(OLD_MODULE) + r"(?![\w-])")

SKIPPED_DIRS = {".git", ".gradle", "build", "node_modules"}
SKIPPED_FILES = {"CHANGELOG.md", os.path.basename(__file__)}
TEXT_SUFFIXES = {
    ".java", ".cpp", ".h", ".mm", ".kts", ".md", ".yml", ".yaml", ".json",
    ".txt", ".properties", ".cmake", ".toml", ".js", ".ts", ".mts", ".html",
}


def run(*command):
    subprocess.run(command, check=True)


def package_roots(root):
    """The directories of the old package, one per source set."""
    old_dir = os.sep + OLD_PACKAGE.replace(".", os.sep)
    roots = []

    for directory, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in SKIPPED_DIRS]

        if directory.endswith(old_dir):
            roots.append(directory)
            # The whole tree below moves.
            dirs[:] = []

    return roots


def tracked(path):
    return subprocess.run(["git", "ls-files", "--error-unmatch", path], capture_output=True).returncode == 0


def move_files(root):
    """Moves the trees below the old package directories into the new ones."""
    old_dir = OLD_PACKAGE.replace(".", os.sep)
    new_dir = NEW_PACKAGE.replace(".", os.sep)
    moved = 0

    for old_root in package_roots(root):
        base = old_root[:-len(old_dir)]
        new_root = base + new_dir

        for directory, dirs, files in os.walk(old_root):
            for name in files:
                source = os.path.join(directory, name)
                target = os.path.join(new_root, os.path.relpath(source, old_root))
                os.makedirs(os.path.dirname(target), exist_ok=True)
                if tracked(source):
                    run("git", "mv", "-k", source, target)
                else:
                    os.replace(source, target)
                moved += 1

        # Drop the emptied directories, up to the first segment of the old package.
        for directory, dirs, files in os.walk(old_root, topdown=False):
            os.rmdir(directory)
        parent = os.path.dirname(old_root)
        while len(parent) > len(base) and not os.listdir(parent):
            os.rmdir(parent)
            parent = os.path.dirname(parent)

    return moved


def rewrite_files(root):
    """Rewrites the package, module and JNI symbol names in the text files."""
    rewritten = 0

    for directory, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in SKIPPED_DIRS]

        for name in files:
            if name in SKIPPED_FILES or os.path.splitext(name)[1] not in TEXT_SUFFIXES:
                continue

            path = os.path.join(directory, name)
            with open(path, "rb") as file:
                original = file.read()

            text = original.decode("utf-8")
            for old, new in REPLACEMENTS:
                text = text.replace(old, new)
            text = MODULE_PATTERN.sub(NEW_MODULE, text)

            if text.encode("utf-8") != original:
                with open(path, "wb") as file:
                    file.write(text.encode("utf-8"))
                rewritten += 1

    return rewritten


def main():
    root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
    os.chdir(root)

    moved = move_files(root)
    rewritten = rewrite_files(root)

    print(f"{moved} files moved, {rewritten} files rewritten")


if __name__ == "__main__":
    main()
