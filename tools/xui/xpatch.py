#!/usr/bin/env python3
"""XUIPAT1 binary COPY/INSERT patches. Runtime apply() uses only stdlib.

The generator recognizes XUIZ packages and matches unchanged entries as whole
COPY runs, then diffs changed entries against their corresponding originals.
See docs/xui-format.md (Patch format) for the exact portable wire layout.
"""
import argparse
import hashlib
from pathlib import Path
import struct

MAGIC = b'XUIPAT1\0'
HEADER = struct.Struct('>8sQQ32s32sI')
COPY, INSERT = 0, 1
MIN_COPY = 16


def digest(data):
    return hashlib.sha256(data).digest()


def decode(patch):
    """Strict format reader shared by the applier and literal audit."""
    if len(patch) < HEADER.size:
        raise ValueError('truncated patch header')
    magic, old_size, new_size, old_hash, new_hash, count = HEADER.unpack_from(patch)
    if magic != MAGIC or count > (len(patch)-HEADER.size)//9:
        raise ValueError('unsupported patch or impossible operation count')
    pos, produced, ops = HEADER.size, 0, []
    for _ in range(count):
        if pos + 9 > len(patch):
            raise ValueError('truncated operation')
        kind = patch[pos]
        if kind == COPY:
            if pos + 17 > len(patch):
                raise ValueError('truncated COPY')
            offset, size = struct.unpack_from('>QQ', patch, pos+1)
            pos += 17
            if offset > old_size or size > old_size-offset:
                raise ValueError('COPY exceeds original bounds')
            op = (COPY, offset, size)
        elif kind == INSERT:
            size = struct.unpack_from('>Q', patch, pos+1)[0]
            pos += 9
            if size > len(patch)-pos:
                raise ValueError('truncated INSERT')
            op = (INSERT, patch[pos:pos+size])
            pos += size
        else:
            raise ValueError('unknown opcode')
        if not size or size > new_size-produced:
            raise ValueError('zero length or overflowing operation')
        produced += size
        ops.append(op)
    if pos != len(patch) or produced != new_size:
        raise ValueError('trailing patch bytes or incorrect result size')
    return (old_size, new_size, old_hash, new_hash), ops


def apply(original, patch):
    """Verify both ends; no game parser or generator dependency is required."""
    (old_size, new_size, old_hash, new_hash), ops = decode(patch)
    if len(original) != old_size or digest(original) != old_hash:
        raise ValueError('original size/SHA256 mismatch')
    result = bytearray()
    for op in ops:
        result.extend(original[op[1]:op[1]+op[2]] if op[0] == COPY else op[1])
    if len(result) != new_size or digest(result) != new_hash:
        raise ValueError('result size/SHA256 mismatch')
    return bytes(result)


def coalesce(ops):
    result = []
    for op in ops:
        if (op[2] if op[0] == COPY else len(op[1])) == 0:
            continue
        if result and op[0] == result[-1][0] == INSERT:
            result[-1] = (INSERT, result[-1][1] + op[1])
        elif result and op[0] == result[-1][0] == COPY and result[-1][1]+result[-1][2] == op[1]:
            result[-1] = (COPY, result[-1][1], result[-1][2]+op[2])
        else:
            result.append(op)
    return result


def diff_region(old, new, offset):
    from difflib import SequenceMatcher
    ops, cursor = [], 0
    for block in SequenceMatcher(None, old, new, autojunk=False).get_matching_blocks():
        # gaps in the result are literals; deleted source bytes need no opcode.
        if block.b > cursor:
            ops.append((INSERT, new[cursor:block.b]))
        if block.size:
            ops.append((COPY, offset+block.a, block.size))
        cursor = block.b + block.size
    return ops


def package_ops(original, result, aliases):
    try:
        from . import xzp
    except ImportError:
        import xzp
    old_entries, new_entries = xzp.parse(original), xzp.parse(result)
    old_start = 22 + struct.unpack_from('>I', original, 16)[0]
    new_start = 22 + struct.unpack_from('>I', result, 16)[0]
    # Diff each small directory record independently. A byte-level diff over
    # the entire repetitive directory can become quadratic on large packages.
    ops = diff_region(original[:22], result[:22], 0)
    directory, cursor = {}, 22
    for entry in old_entries:
        size = 9 + len(entry.name.encode('utf-16-be'))
        directory.setdefault(entry.name, []).append((cursor, original[cursor:cursor+size]))
        cursor += size
    cursor = 22
    for entry in new_entries:
        size = 9 + len(entry.name.encode('utf-16-be'))
        record = result[cursor:cursor+size]
        candidates = directory.get(aliases.get(entry.name, entry.name), [])
        if candidates:
            offset, old_record = next((c for c in candidates if c[1] == record), candidates[0])
            ops.extend(diff_region(old_record, record, offset))
        else:
            ops.append((INSERT, record))
        cursor += size
    if cursor != new_start:
        raise ValueError('directory size mismatch')
    for entry in new_entries:
        source_name = aliases.get(entry.name, entry.name)
        candidates = [e for e in old_entries if e.name == source_name]
        exact = next((e for e in candidates if e.data == entry.data), None)
        if exact:
            ops.append((COPY, old_start+exact.offset, exact.size))
        elif len(candidates) == 1:
            source = candidates[0]
            ops.extend(diff_region(source.data, entry.data, old_start+source.offset))
        else:
            ops.append((INSERT, entry.data))
    return ops


def reuse_literals(original, ops, threshold=MIN_COPY):
    """Replace every original-matching >=16-byte literal span with COPY.

    Exhaustive window lookup includes unaligned offsets and other entries, so
    reused data cannot leak into long INSERTs even when diff picks a poor match.
    This pass is cheap for package patches: only small edit gaps are searched.
    """
    result = []
    for op in ops:
        if op[0] == COPY:
            result.append(op)
            continue
        data, pos, pending = op[1], 0, 0
        while pos + threshold <= len(data):
            offset = original.find(data[pos:pos+threshold])
            if offset < 0:
                pos += 1
                continue
            if pending < pos:
                result.append((INSERT, data[pending:pos]))
            size = threshold
            while pos+size < len(data) and offset+size < len(original) and data[pos+size] == original[offset+size]:
                size += 1
            result.append((COPY, offset, size))
            pos += size
            pending = pos
        if pending < len(data):
            result.append((INSERT, data[pending:]))
    return coalesce(result)


def generate(original, result, aliases=None):
    if original.startswith(b'XUIZ') and result.startswith(b'XUIZ'):
        ops = package_ops(original, result, aliases or {})
    else:
        # Bounded block index for arbitrary large inputs; matching is independent
        # of target alignment. The package path avoids indexing large textures.
        blocks = {}
        for i in range(0, len(original)-MIN_COPY+1, MIN_COPY):
            blocks.setdefault(original[i:i+MIN_COPY], i)
        ops, pos, pending = [], 0, 0
        while pos + MIN_COPY <= len(result):
            offset = blocks.get(result[pos:pos+MIN_COPY])
            if offset is None:
                pos += 1
                continue
            if pending < pos:
                ops.append((INSERT, result[pending:pos]))
            size = MIN_COPY
            while pos+size < len(result) and offset+size < len(original) and result[pos+size] == original[offset+size]:
                size += 1
            ops.append((COPY, offset, size))
            pos += size
            pending = pos
        if pending < len(result):
            ops.append((INSERT, result[pending:]))
    ops = reuse_literals(original, coalesce(ops))
    body = bytearray()
    for op in ops:
        if op[0] == COPY:
            body += struct.pack('>BQQ', COPY, op[1], op[2])
        else:
            body += struct.pack('>BQ', INSERT, len(op[1])) + op[1]
    patch = HEADER.pack(MAGIC, len(original), len(result), digest(original), digest(result), len(ops)) + body
    if apply(original, patch) != result:
        raise ValueError('generated patch verification failed')
    return patch


def audit(original, patch, threshold=MIN_COPY):
    """Report counts plus any original-matching literal windows, without dumps."""
    info, ops = decode(patch)
    if len(original) != info[0] or digest(original) != info[2]:
        raise ValueError('audit original mismatch')
    inserts = [op[1] for op in ops if op[0] == INSERT]
    matches = sum(original.find(data[i:i+threshold]) >= 0
                  for data in inserts for i in range(len(data)-threshold+1))
    return dict(patch_bytes=len(patch), literal_bytes=sum(map(len, inserts)),
                copy_bytes=sum(op[2] for op in ops if op[0] == COPY),
                operations=len(ops), longest_insert=max(map(len, inserts), default=0),
                original_match_threshold=threshold, original_matching_literal_windows=matches)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    for command in ('generate', 'apply', 'audit'):
        p = sub.add_parser(command)
        p.add_argument('original', type=Path)
        p.add_argument('other', type=Path, help='modified file for generate; patch otherwise')
        if command != 'audit':
            p.add_argument('output', type=Path)
    args = parser.parse_args()
    original, other = args.original.read_bytes(), args.other.read_bytes()
    if args.command == 'audit':
        print(audit(original, other))
    else:
        # CLI writes stay in the ignored output tree. apply() itself is portable.
        try:
            from .xzp import output_root, write_output
        except ImportError:
            from xzp import output_root, write_output
        target = output_root(args.output)
        data = generate(original, other) if args.command == 'generate' else apply(original, other)
        if target == args.original.resolve():
            raise ValueError('output would replace the original')
        write_output(target, data)
        if args.command == 'generate':
            print(audit(original, data))


if __name__ == '__main__':
    main()
