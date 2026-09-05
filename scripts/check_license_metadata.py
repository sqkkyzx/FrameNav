#!/usr/bin/env python3
# Copyright 2026 FrameNav contributors
# SPDX-License-Identifier: Apache-2.0

"""Check the repository's required Apache-2.0 release metadata."""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
REQUIRED_DOCUMENTS = (
    "LICENSE",
    "NOTICE",
    "MODIFICATIONS.md",
    "THIRD_PARTY_NOTICES.md",
    "CONTRIBUTING.md",
)


def main() -> int:
    errors: list[str] = []

    for relative in REQUIRED_DOCUMENTS:
        path = ROOT / relative
        if not path.is_file() or not path.read_text(encoding="utf-8").strip():
            errors.append(f"missing or empty: {relative}")

    license_path = ROOT / "LICENSE"
    if license_path.is_file():
        license_text = license_path.read_text(encoding="utf-8")
        if "Apache License" not in license_text or "Version 2.0" not in license_text:
            errors.append("LICENSE is not the full Apache License 2.0 text")

    source_paths = sorted((ROOT / "src").glob("*.[ch]"))
    source_paths += sorted((ROOT / "include").glob("*.[ch]"))
    source_paths += sorted((ROOT / "test").glob("*.py"))
    source_paths += sorted((ROOT / "scripts").glob("*.py"))

    for path in source_paths:
        if path.name == "fake_clock_seed.generated.h":
            continue
        text = path.read_text(encoding="utf-8")
        if "SPDX-License-Identifier: Apache-2.0" not in text[:1024]:
            errors.append(f"missing SPDX header: {path.relative_to(ROOT)}")

    if errors:
        print("License metadata check failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1

    print(f"License metadata OK ({len(source_paths)} source files checked)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
