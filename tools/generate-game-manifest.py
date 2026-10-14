"""Generate metadata only from the owner's supported extracted disc (never copies assets)."""
import argparse
import hashlib
from pathlib import Path

SUPPORTED = "7aaa7a8920d1c3c737e58cf583debafe6013be0a96bec3ec136e5de7e3377e5a"

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--output", type=Path, default=Path(__file__).with_name("game-manifest.txt"))
    args = parser.parse_args()
    files = sorted((p for p in args.folder.rglob("*") if p.is_file() and p.name != ".verified"),
                   key=lambda p: p.relative_to(args.folder).as_posix().lower())
    xex = [p for p in files if p.relative_to(args.folder).as_posix().lower() == "default.xex"]
    if len(xex) != 1:
        parser.error("Folder does not contain the supported default.xex")
    with xex[0].open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != SUPPORTED:
            parser.error("Folder does not contain the supported default.xex")
    if len(files) != 197:
        parser.error(f"Expected 197 reference disc files, found {len(files)}")
    lines = ["# Crackdown World disc; path<TAB>bytes<TAB>SHA256 (metadata only, no game data)"]
    for path in files:
        if path.is_symlink():
            parser.error(f"Symlink in game folder: {path}")
        relative = path.relative_to(args.folder).as_posix()
        size = path.stat().st_size
        with path.open("rb") as stream:
            # Every file: the launcher hashes all of them once after an import or folder selection.
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        lines.append(f"{relative}\t{size}\t{digest}")
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{len(files)} files, {sum(p.stat().st_size for p in files):,} bytes; wrote {args.output}")

if __name__ == "__main__":
    main()
