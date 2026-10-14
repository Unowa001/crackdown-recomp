#!/usr/bin/env python3
"""Read, extract and rebuild big-endian XUIZ v1 packages (stdlib only)."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import struct
import sys
import tempfile


class FormatError(ValueError):
    pass


@dataclass(frozen=True)
class Entry:
    name: str
    data: bytes
    offset: int = 0  # original offset relative to the payload start

    @property
    def size(self):
        return len(self.data)


def parse(data: bytes) -> list[Entry]:
    """Return ordered entries; reject unsupported layouts rather than lose bytes."""
    if len(data) < 22:
        raise FormatError('truncated XUIZ header')
    magic, version, size, reserved, table_size, count = struct.unpack_from('>4sIIIIH', data)
    if magic != b'XUIZ' or version != 1 or reserved != 0:
        raise FormatError('expected XUIZ v1 with reserved=0')
    if size != len(data) or 22 + table_size > size:
        raise FormatError('invalid file/table size')
    pos, payload, end = 22, 22 + table_size, 0
    entries = []
    for _ in range(count):
        if pos + 9 > payload:
            raise FormatError('truncated entry table')
        length, offset, chars = struct.unpack_from('>IIB', data, pos)
        pos += 9
        if pos + 2 * chars > payload:
            raise FormatError('truncated entry name')
        try:
            name = data[pos:pos + 2 * chars].decode('utf-16-be')
        except UnicodeError as exc:
            raise FormatError('invalid UTF-16BE name') from exc
        pos += 2 * chars
        if offset != end or payload + offset + length > size:
            raise FormatError('non-contiguous or out-of-bounds payload')
        entries.append(Entry(name, data[payload + offset:payload + offset + length], offset))
        end = offset + length
    if pos != payload or payload + end != size:
        raise FormatError('unaccounted table/payload bytes')
    return entries


def read(path: str | Path) -> list[Entry]:
    return parse(Path(path).read_bytes())


def get(entries: list[Entry], name: str) -> bytes:
    """Exact, case-sensitive lookup. Duplicates are an error."""
    matches = [entry.data for entry in entries if entry.name == name]
    if len(matches) != 1:
        raise KeyError(name)
    return matches[0]


def build(entries) -> bytes:
    """Pack ordered Entry objects or (name, bytes) pairs into canonical XUIZ v1."""
    entries = list(entries)
    table, payload = bytearray(), bytearray()
    if len(entries) > 65535:
        raise FormatError('too many entries')
    for entry in entries:
        name, data = (entry.name, entry.data) if isinstance(entry, Entry) else entry
        name_bytes = name.encode('utf-16-be')
        chars = len(name_bytes) // 2  # UTF-16 code units, not Python code points
        if chars > 255:
            raise FormatError('entry name exceeds 255 UTF-16 code units')
        table += struct.pack('>IIB', len(data), len(payload), chars) + name_bytes
        payload += data
    return struct.pack('>4sIIIIH', b'XUIZ', 1, 22 + len(table) + len(payload), 0,
                       len(table), len(entries)) + table + payload


def output_root(path: str | Path) -> Path:
    """Constrain game-derived output to this worktree's ignored out/xui tree."""
    root = Path(__file__).resolve().parents[2] / 'out' / 'xui'
    target = Path(path).resolve()
    if not target.is_relative_to(root.resolve()):
        raise FormatError(f'output must be under {root}')
    return target


def write_output(path, data):
    """Publish a derived artifact under out/xui without truncating hardlinks."""
    target = output_root(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
        temporary.replace(target)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def extract(entries, outdir: str | Path) -> None:
    root = output_root(outdir)
    targets = {}
    for entry in entries:
        name = entry.name.replace('\\', '/')
        if name.startswith('/') or ':' in name or any(p in ('', '.', '..') for p in name.split('/')):
            raise FormatError(f'unsafe entry path: {entry.name!r}')
        target = (root / name).resolve()
        if not target.is_relative_to(root):
            raise FormatError(f'escaping entry path: {entry.name!r}')
        if target in targets and targets[target].data != entry.data:
            raise FormatError(f'conflicting duplicate entry path: {entry.name!r}')
        targets[target] = entry
    for target, entry in targets.items():
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            if target.is_file() and target.read_bytes() == entry.data:
                continue
            raise FormatError(f'output already exists with different content: {target}')
        # Exclusive creation avoids clobbering existing files and symlinks.
        with target.open('xb') as stream:
            stream.write(entry.data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    for command in ('list', 'roundtrip', 'extract'):
        p = sub.add_parser(command)
        p.add_argument('file', type=Path)
        if command == 'extract':
            p.add_argument('outdir', type=Path)
    args = parser.parse_args()
    try:
        data = args.file.read_bytes()
        entries = parse(data)
        if args.command == 'list':
            for entry in entries:
                print(f'{entry.size:10d}  +0x{entry.offset:08x}  {entry.name}')
        elif args.command == 'extract':
            extract(entries, args.outdir)
            print(f'extracted {len(entries)} entries to {args.outdir}')
        else:
            if build(entries) != data:
                raise FormatError('rebuilt bytes differ')
            print(f'OK: {len(entries)} entries, {len(data)} bytes, byte-identical')
    except (OSError, ValueError) as exc:
        print(f'{args.file}: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
