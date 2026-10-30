#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Convert the local FrameNav PEM certificate and key into a C source file.

The generated file is intentionally kept under the ignored certs/ directory:
the firmware needs the private key, but the source repository must not carry it.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import tempfile


def read_pem(path: Path, label: str) -> bytes:
    data = path.read_bytes()
    if not data or b"\0" in data:
        raise SystemExit(f"{label} is empty or contains a NUL: {path}")
    if label == "certificate":
        markers = (b"-----BEGIN CERTIFICATE-----", b"-----END CERTIFICATE-----")
    else:
        markers = (b"-----BEGIN", b"PRIVATE KEY-----")
    if not all(marker in data for marker in markers):
        raise SystemExit(f"{label} is not a PEM file: {path}")
    if label == "certificate" and b"PRIVATE KEY" in data:
        raise SystemExit(f"certificate unexpectedly contains a private key: {path}")
    return data


def c_array(name: str, data: bytes) -> str:
    data += b"\0"  # Mbed TLS requires a NUL, included in the PEM buffer size.
    lines = [f"const unsigned char {name}[] = {{"]
    for offset in range(0, len(data), 12):
        chunk = data[offset : offset + 12]
        lines.append("  " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    lines.append("};")
    lines.append(f"const unsigned int {name}_len = {len(data)};")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cert", type=Path, required=True)
    parser.add_argument("--key", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    cert = read_pem(args.cert, "certificate")
    key = read_pem(args.key, "private key")
    output = "/* SPDX-License-Identifier: Apache-2.0 */\n\n"
    output += c_array("framenav_server_cert", cert)
    output += "\n"
    output += c_array("framenav_server_key", key)
    output += "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # The generated source contains a private key. Create it with mode 0600
    # from the start, and replace atomically so interrupted builds cannot use
    # a partially generated array.
    with tempfile.NamedTemporaryFile(mode="w", encoding="ascii", newline="\n",
                                     dir=args.output.parent, delete=False) as file:
        temporary = Path(file.name)
        try:
            file.write(output)
            file.close()
            os.replace(temporary, args.output)
        finally:
            temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
