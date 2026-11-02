#!/usr/bin/env python3
"""Check Markdown whitespace, closed fences and local link files without network access."""

import argparse
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote, urlsplit

from markdown_it import MarkdownIt


def links(tokens):
    """Yield Markdown link and image destinations, including reference links."""
    for token in tokens:
        if token.type in {"link_open", "image"}:
            yield token.attrGet("href" if token.type == "link_open" else "src")
        if token.children:
            yield from links(token.children)


def check_document(root, path):
    """Return errors for one document; external URLs and fragments are not fetched."""
    errors = []
    if path.is_symlink():
        return ["Markdown must be a regular file"]
    data = path.read_bytes()
    text = data.decode("utf-8")
    if b"\r" in data or (data and not data.endswith(b"\n")):
        errors.append("Use LF line endings and a final newline")
    lines = text.split("\n")
    for number, line in enumerate(lines, 1):
        suffix = line[len(line.rstrip()) :]
        if suffix and (suffix != "  " or not line.strip()):
            errors.append(f"Line {number}: trailing whitespace")
    tokens = MarkdownIt("commonmark").parse(text)
    for token in tokens:
        if token.type == "fence":
            closing = lines[token.map[1] - 1]
            pattern = (
                rf"[ >\t]*{re.escape(token.markup[0])}{{{len(token.markup)},}}[ \t]*"
            )
            if token.map[1] - token.map[0] < 2 or not re.fullmatch(pattern, closing):
                errors.append(f"Line {token.map[0] + 1}: unclosed code fence")
    for destination in links(tokens):
        url = urlsplit(destination)
        if url.scheme or url.netloc or not url.path:
            continue
        relative = unquote(url.path)
        target = (
            (root / relative.lstrip("/"))
            if relative.startswith("/")
            else path.parent / relative
        )
        target = target.resolve()
        if not target.is_relative_to(root) or not target.exists():
            errors.append(f"Missing local link target: {destination}")
    return errors


def main():
    """Check every tracked Markdown document and report file-specific errors."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    root = parser.parse_args().root.resolve()
    paths = (
        subprocess.check_output(
            ["git", "-C", str(root), "ls-files", "-z", "--", "*.md"]
        )
        .decode()
        .split("\0")
    )
    failed = False
    for name in filter(None, paths):
        for error in check_document(root, root / name):
            print(f"{name}: {error}")
            failed = True
    if failed:
        raise SystemExit(1)
    print("Markdown whitespace, code fences and local link files passed")


if __name__ == "__main__":
    main()
