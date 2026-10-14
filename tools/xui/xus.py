#!/usr/bin/env python3
"""Read companion XUIS v1 localization property overrides (stdlib only)."""
from pathlib import Path
import struct

try:
    from .xur import Reader, FormatError
except ImportError:
    from xur import Reader, FormatError


def parse(data):
    r = Reader(data)
    if r.take(4) != b'XUIS' or r.take(2) != b'\x01\x00':
        raise FormatError('expected XUIS v1')
    size, count = r.unpack('I'), r.unpack('H')
    if size != len(data):
        raise FormatError('XUIS file size mismatch')
    overrides = []
    for _ in range(count):
        offset = r.pos
        value = r.take(r.unpack('H') * 2).decode('utf-16-be')
        path = r.take(r.unpack('H') * 2).decode('utf-16-be')
        overrides.append(dict(offset=offset, value=value, path=path))
    if r.pos != r.end:
        raise FormatError(f'unconsumed XUIS bytes at 0x{r.pos:x}')
    return overrides


def read(path):
    return parse(Path(path).read_bytes())


def build(overrides):
    body = bytearray()
    for override in overrides:
        for key in ('value', 'path'):
            raw = override[key].encode('utf-16-be')
            body += struct.pack('>H', len(raw)//2) + raw
    return b'XUIS' + b'\x01\x00' + struct.pack('>IH', 12+len(body), len(overrides)) + body
