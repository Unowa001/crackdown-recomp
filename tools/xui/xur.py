#!/usr/bin/env python3
"""Inspect XUIB v5 scenes without changing input files. Python stdlib only."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct
import sys

try:
    from .schema import CLASSES, hierarchy
except ImportError:
    from schema import CLASSES, hierarchy


class FormatError(ValueError):
    pass


class Reader:
    def __init__(self, data, start=0, end=None):
        self.data, self.pos = data, start
        self.end = len(data) if end is None else end

    def take(self, size):
        if size < 0 or self.pos + size > self.end:
            raise FormatError(f'truncated record at 0x{self.pos:x} (need {size} bytes)')
        start = self.pos
        self.pos += size
        return self.data[start:self.pos]

    def unpack(self, fmt):
        return struct.unpack('>' + fmt, self.take(struct.calcsize('>' + fmt)))[0]

    def count(self):
        n = self.unpack('I')
        if n > self.end - self.pos:
            raise FormatError(f'impossible count {n} at 0x{self.pos - 4:x}')
        return n


def nodes(root):
    yield root
    for child in root['children']:
        yield from nodes(child)


class Scene:
    """Lossless section container plus a decoded DATA tree.

    Indices, masks, flags, offsets and raw value bytes are retained. serialize()
    rebuilds known sections from these records; opaque sections stay raw.
    Indices are authoritative; adding properties requires updating their masks.
    """
    def __init__(self, data):
        self.sections, self.errors = [], []
        self.root = None
        r = Reader(data)
        if r.take(4) != b'XUIB':
            raise FormatError('expected XUIB')
        self.version = r.unpack('I')
        if self.version != 5:
            raise FormatError(f'unsupported XUR version {self.version}')
        self.flags, self.tool_version = r.unpack('I'), r.unpack('H')
        size, count = r.unpack('I'), r.unpack('H')
        if size != len(data):
            raise FormatError('header file size mismatch')
        if self.flags & ~1:
            raise FormatError(f'unknown header flags 0x{self.flags:x}')
        self.count_header = [r.unpack('I') for _ in range(10)] if self.flags & 1 else []
        for _ in range(count):
            tag = r.take(4).decode('ascii')
            offset, length = r.unpack('I'), r.unpack('I')
            if any(s['tag'] == tag for s in self.sections):
                raise FormatError(f'duplicate section {tag}')
            if offset + length > len(data):
                raise FormatError(f'{tag} exceeds file bounds')
            self.sections.append(dict(tag=tag, offset=offset, size=length,
                                      raw=data[offset:offset + length].hex()))
        end = r.pos
        for s in self.sections:
            if s['offset'] < end:
                raise FormatError('overlapping or unordered sections')
            s['gap_before'] = data[end:s['offset']].hex()
            end = s['offset'] + s['size']
        self.trailer = data[end:].hex()
        self.strings = [dict(index=0, offset=None, value='')]
        s = self.section('STRN')
        r = Reader(data, s['offset'], s['offset'] + s['size'])
        while r.pos < r.end:
            start = r.pos
            length = r.unpack('H')
            value = r.take(length * 2).decode('utf-16-be')
            self.strings.append(dict(index=len(self.strings), offset=start, value=value))
        self.vectors, self.quaternions = [], []
        for tag, width, dest in [('VECT', 3, self.vectors), ('QUAT', 4, self.quaternions)]:
            s = self.section(tag, False)
            if s:
                if s['size'] % (4 * width):
                    raise FormatError(f'invalid {tag} size')
                raw = bytes.fromhex(s['raw'])
                for off in range(0, len(raw), 4 * width):
                    dest.append(dict(index=len(dest), offset=s['offset'] + off,
                                     value=list(struct.unpack_from('>' + 'f' * width, raw, off)),
                                     raw=raw[off:off + 4 * width].hex()))
        self.custom = []
        s = self.section('CUST', False)
        if s:
            r = Reader(data, s['offset'], s['offset'] + s['size'])
            while r.pos < r.end:
                offset = r.pos
                length = r.unpack('I')
                raw = r.take(length)
                record = dict(offset=offset, relative_offset=offset-s['offset'], size=length, raw=raw.hex())
                if length >= 12:
                    x, y, n = struct.unpack_from('>ffI', raw)
                    if length == 12 + 24 * n:
                        record.update(bounds=[x, y], points=[list(struct.unpack_from('>6f', raw, 12 + 24*i)) for i in range(n)])
                    else:
                        self.errors.append(f'opaque CUST record at 0x{offset:x}: not a Bezier point block')
                else:
                    self.errors.append(f'opaque CUST record at 0x{offset:x}: shorter than 12 bytes')
                self.custom.append(record)
        for s in self.sections:
            if s['tag'] not in ('STRN', 'VECT', 'QUAT', 'CUST', 'DATA'):
                self.errors.append(f'opaque section {s["tag"]}')
        s = self.section('DATA')
        r = Reader(data, s['offset'], s['offset'] + s['size'])
        try:
            self.root = self.element(r)
            if r.pos != r.end:
                raise FormatError(f'{r.end-r.pos} unconsumed DATA bytes at 0x{r.pos:x}')
        except (ValueError, IndexError, KeyError, RecursionError) as exc:
            self.errors.append(f'{exc} (DATA cursor 0x{r.pos:x})')
        if self.root is not None and not self.errors and self.count_header:
            actual = self.computed_counts()
            if actual != self.count_header:
                self.errors.append(f'extended counts mismatch: {self.count_header} != {actual}')
        self.data_consumed = r.pos - s['offset']
        self.data_tail = data[r.pos:r.end].hex()

    @property
    def fully_parsed(self):
        return not self.errors

    def section(self, tag, required=True):
        for section in self.sections:
            if section['tag'] == tag:
                return section
        if required:
            raise FormatError(f'missing {tag} section')
        return None

    def string(self, index):
        if index >= len(self.strings):
            raise FormatError(f'invalid STRN index {index}')
        return self.strings[index]['value']

    def property(self, r, name, kind, read_array=True):
        start = r.pos
        prop = dict(name=name, type=kind, offset=start)
        if kind.startswith('a'):
            count = r.unpack('B') if read_array else 1
            prop['items'] = [self.property(r, name, kind[1:]) for _ in range(count)]
            prop['value'] = [p['value'] for p in prop['items']]
        elif kind in ('i', 'u', 'f', 'b'):
            value = r.unpack({'i':'i', 'u':'I', 'f':'f', 'b':'B'}[kind])
            prop['value'] = bool(value) if kind == 'b' else value
        elif kind == 'c':
            prop['value'] = '#' + r.take(4).hex()  # ARGB
        elif kind == 's':
            index = r.unpack('H')
            prop.update(index=index, value=self.string(index))
        elif kind in ('v', 'q'):
            index = r.unpack('I')
            table = self.vectors if kind == 'v' else self.quaternions
            if index >= len(table):
                raise FormatError(f'invalid {kind} index {index} at 0x{start:x}')
            prop.update(index=index, value=table[index]['value'])
        elif kind == 'r':
            off = r.unpack('I')
            matches = [c for c in self.custom if c['relative_offset'] == off]
            if not matches:
                raise FormatError(f'invalid CUST offset {off} at 0x{start:x}')
            prop.update(index=off, value=matches[0])
        elif kind == 'o':
            cls = 'XuiFigureFillGradient' if name == 'Gradient' else 'XuiFigure' + name
            count, packed = r.unpack('H'), r.unpack('B')
            fields = CLASSES[cls][1]
            mask = r.take(max(1, (len(fields)+7)//8))
            items = self.mask_properties(r, cls, int.from_bytes(mask, 'big'))
            actual = sum(len(p['items']) if p['type'].startswith('a') else 1 for p in items)
            if actual != count:
                raise FormatError(f'compound {name} count {count} != {actual} at 0x{start:x}')
            prop.update(count=count, packed=packed, mask=mask.hex(), items=items,
                        value={p['name']: p['value'] for p in items})
        else:
            raise FormatError(f'unknown property {name}:{kind} at 0x{start:x}')
        prop['raw'] = r.data[start:r.pos].hex()
        return prop

    def mask_properties(self, r, cls, mask):
        fields = CLASSES[cls][1]
        if mask >> len(fields):
            raise FormatError(f'unknown property bits 0x{mask:x} in {cls} at 0x{r.pos:x}')
        return [self.property(r, name, kind) for i, (name, kind) in enumerate(fields) if mask & (1 << i)]

    def element(self, r):
        start = r.pos
        index, flags = r.unpack('H'), r.unpack('B')
        cls = self.string(index)
        obj = dict(offset=start, class_index=index, **{'class':cls}, flags=flags,
                   properties=[], property_groups=[], children=[], named_frames=[], timelines=[])
        if flags & ~7:
            raise FormatError(f'unknown element flags 0x{flags:x} at 0x{start:x}')
        if flags & 1:
            obj['property_count'] = r.unpack('H')
            for ancestor in reversed(hierarchy(cls)):
                off = r.pos
                packed = r.unpack('B')
                if (packed & 7) not in (0, 1, 2, 4):
                    raise FormatError(f'unsupported property mask width at 0x{off:x}')
                mask_bytes = r.take(packed & 7)
                mask = int.from_bytes(mask_bytes, 'big')
                props = self.mask_properties(r, ancestor, mask)
                if packed and len(props) != (packed >> 3) + 1:
                    raise FormatError(f'packed property count mismatch in {ancestor} at 0x{off:x}')
                obj['property_groups'].append(dict(class_name=ancestor, offset=off, packed=packed,
                                                   mask=mask_bytes.hex(), properties=props))
                obj['properties'].extend(props)
            if len(obj['properties']) != obj['property_count']:
                raise FormatError(f'property count mismatch at 0x{start:x}')
        if flags & 2:
            obj['children'] = [self.element(r) for _ in range(r.count())]
        if flags & 4:
            for _ in range(r.count()):
                off = r.pos
                name_index, frame, command, target_index = r.unpack('H'), r.unpack('I'), r.unpack('B'), r.unpack('H')
                obj['named_frames'].append(dict(offset=off, name_index=name_index, name=self.string(name_index),
                                                frame=frame, command=command, target_index=target_index,
                                                target=None if target_index == 65535 else self.string(target_index)))
            if obj['children']:
                for _ in range(r.count()):
                    obj['timelines'].append(self.timeline(r, obj))
        obj['id'] = next((p['value'] for p in obj['properties'] if p['name'] == 'Id'), '')
        obj['size'] = r.pos - start
        return obj

    def timeline(self, r, owner):
        start = r.pos
        index = r.unpack('H')
        target = self.string(index)
        candidates = [n for n in nodes(owner) if any(p['name']=='Id' and p['value']==target for p in n['properties'])]
        if not candidates:
            raise FormatError(f'timeline target {target!r} missing at 0x{start:x}')
        chain = hierarchy(candidates[0]['class'])
        definitions = []
        for _ in range(r.count()):
            off = r.pos
            packed, class_index = r.unpack('B'), r.unpack('B')
            depth = packed & 127
            if not depth or class_index >= len(chain):
                raise FormatError(f'invalid timeline property path at 0x{off:x}')
            cls, path = chain[class_index], []
            for i in range(depth):
                bit = r.unpack('B')
                fields = CLASSES[cls][1]
                if bit >= len(fields):
                    raise FormatError(f'unknown animated property {cls}[{bit}] at 0x{r.pos-1:x}')
                name, kind = fields[bit]
                path.append(bit)
                if i < depth-1:
                    cls = 'XuiFigureFillGradient' if name == 'Gradient' else 'XuiFigure' + name
            array_index = r.unpack('I') if packed & 128 else None
            definitions.append(dict(offset=off, packed=packed, class_index=class_index, path=path,
                                     name=name, type=kind, array_index=array_index))
        keyframes = []
        for _ in range(r.count()):
            off = r.pos
            frame = r.unpack('I')
            interpolation = list(r.take(4))
            props = [self.property(r, d['name'], d['type'], False) for d in definitions]
            keyframes.append(dict(offset=off, frame=frame, interpolation=interpolation, properties=props))
        return dict(offset=start, target_index=index, target=target, definitions=definitions, keyframes=keyframes)

    def computed_counts(self):
        def measure(prop):
            if prop['type'] == 'o':
                parts = [measure(p) for p in prop['items']]
                return 1 + sum(p[0] for p in parts), 1 + sum(p[1] for p in parts)
            if prop['type'].startswith('a'):
                return len(prop['items']), 0
            return 1, 0
        elements = list(nodes(self.root))
        properties = [measure(p) for obj in elements for p in obj['properties']]
        timelines = [t for obj in elements for t in obj['timelines']]
        definitions = [d for t in timelines for d in t['definitions']]
        keyframes = [k for t in timelines for k in t['keyframes']]
        return [len(elements), sum(p[0] for p in properties),
                sum(bool(obj['flags'] & 1) for obj in elements) + sum(p[1] for p in properties),
                sum(len(k['properties']) for k in keyframes),
                sum(len(d['path']) for d in definitions), len(definitions), len(keyframes),
                len(timelines), sum(len(obj['named_frames']) for obj in elements),
                sum(bool(obj['children']) for obj in elements)]

    def serialize(self):
        """Re-encode known sections, retaining original indices and masks.

        String/vector/quaternion/custom references use index, not the resolved
        inspection value. Edit their owning tables. Opaque sections are copied;
        an incomplete DATA parse gets opaque preservation, not a semantic write.
        """
        payloads = {}
        payloads['STRN'] = b''.join(struct.pack('>H', len(s['value'].encode('utf-16-be'))//2) +
                                   s['value'].encode('utf-16-be') for s in self.strings[1:])
        for tag, table in [('VECT', self.vectors), ('QUAT', self.quaternions)]:
            payloads[tag] = b''.join(encode_floats(item['value'], bytes.fromhex(item['raw'])) for item in table)
        if self.root is not None and self.fully_parsed:
            payloads['DATA'] = encode_element(self.root)
            payloads['CUST'] = b''.join(encode_custom(record) for record in self.custom)
        else:
            # Explicit opaque preservation, never reported as a semantic roundtrip.
            payloads['DATA'] = bytes.fromhex(self.section('DATA')['raw'])
        header_size = 20 + 4*len(self.count_header) + 12*len(self.sections)
        offset, table, body = header_size, bytearray(), bytearray()
        for s in self.sections:
            gap = bytes.fromhex(s['gap_before'])
            body += gap
            offset += len(gap)
            payload = payloads.get(s['tag'], bytes.fromhex(s['raw']))
            table += struct.pack('>4sII', s['tag'].encode('ascii'), offset, len(payload))
            body += payload
            offset += len(payload)
        body += bytes.fromhex(self.trailer)
        return (struct.pack('>4sIIHIH', b'XUIB', self.version, self.flags, self.tool_version,
                            header_size+len(body), len(self.sections)) +
                b''.join(struct.pack('>I', n) for n in (self.computed_counts() if self.count_header and self.fully_parsed else self.count_header)) + table + body)

    def as_dict(self):
        return dict(version=self.version, flags=self.flags, tool_version=self.tool_version,
                    count_header=self.count_header, sections=self.sections, strings=self.strings,
                    vectors=self.vectors, quaternions=self.quaternions, custom=self.custom,
                    fully_parsed=self.fully_parsed, errors=self.errors, root=self.root,
                    data_consumed=self.data_consumed, data_tail=self.data_tail, trailer=self.trailer)


def encode_floats(values, raw):
    # Python may quiet signaling NaNs. Preserve their original payload bits.
    return b''.join(raw[i*4:i*4+4] if math.isnan(value) else struct.pack('>f', value)
                    for i, value in enumerate(values))


def encode_custom(record):
    if 'points' not in record:
        return struct.pack('>I', record['size']) + bytes.fromhex(record['raw'])
    raw = bytes.fromhex(record['raw'])
    body = encode_floats(record['bounds'], raw[:8]) + struct.pack('>I', len(record['points']))
    for i, point in enumerate(record['points']):
        body += encode_floats(point, raw[12+24*i:36+24*i])
    return struct.pack('>I', len(body)) + body


def encode_property(prop, write_array=True):
    kind = prop['type']
    if kind.startswith('a'):
        body = b''.join(encode_property(item) for item in prop['items'])
        return (bytes([len(prop['items'])]) if write_array else b'') + body
    if kind in ('s', 'v', 'q', 'r'):
        return struct.pack('>H' if kind == 's' else '>I', prop['index'])
    if kind == 'o':
        return (struct.pack('>HB', prop['count'], prop['packed']) + bytes.fromhex(prop['mask']) +
                b''.join(encode_property(item) for item in prop['items']))
    if kind == 'f':
        return encode_floats([prop['value']], bytes.fromhex(prop['raw']))
    if kind == 'c':
        return bytes.fromhex(prop['value'].removeprefix('#'))
    if kind in ('i', 'u', 'b'):
        return struct.pack({'i':'>i', 'u':'>I', 'b':'>B'}[kind], prop['value'])
    raise FormatError(f'cannot encode property type {kind!r}')


def encode_element(obj):
    result = bytearray(struct.pack('>HB', obj['class_index'], obj['flags']))
    if obj['flags'] & 1:
        result += struct.pack('>H', obj['property_count'])
        for group in obj['property_groups']:
            result += bytes([group['packed']]) + bytes.fromhex(group['mask'])
            for prop in group['properties']:
                result += encode_property(prop)
    if obj['flags'] & 2:
        result += struct.pack('>I', len(obj['children']))
        for child in obj['children']:
            result += encode_element(child)
    if obj['flags'] & 4:
        result += struct.pack('>I', len(obj['named_frames']))
        for frame in obj['named_frames']:
            result += struct.pack('>HIBH', frame['name_index'], frame['frame'], frame['command'], frame['target_index'])
        if obj['children']:
            result += struct.pack('>I', len(obj['timelines']))
            for timeline in obj['timelines']:
                result += struct.pack('>HI', timeline['target_index'], len(timeline['definitions']))
                for d in timeline['definitions']:
                    result += bytes([d['packed'], d['class_index'], *d['path']])
                    if d['array_index'] is not None:
                        result += struct.pack('>I', d['array_index'])
                result += struct.pack('>I', len(timeline['keyframes']))
                for keyframe in timeline['keyframes']:
                    result += struct.pack('>I', keyframe['frame']) + bytes(keyframe['interpolation'])
                    for prop in keyframe['properties']:
                        result += encode_property(prop, False)
    return bytes(result)


def parse(data: bytes) -> Scene:
    return Scene(data)


def read(path: str | Path) -> Scene:
    return parse(Path(path).read_bytes())


def dump(scene):
    lines = [f'XUIB version={scene.version} tool={scene.tool_version} flags=0x{scene.flags:x}',
             f'Fully parsed: {scene.fully_parsed}']
    if scene.count_header:
        lines.append(f'Extended counts: {scene.count_header}')
    for s in scene.sections:
        lines.append(f'{s["tag"]} @0x{s["offset"]:x} size=0x{s["size"]:x}')
    def walk(obj, depth=0):
        prefix = '  ' * depth
        lines.append(f'{prefix}{obj["class"]} Id={obj["id"]!r} @0x{obj["offset"]:x} flags=0x{obj["flags"]:x}')
        for group in obj['property_groups']:
            lines.append(f'{prefix}  [{group["class_name"]} packed={group["packed"]:02x} mask={group["mask"]}]')
            for prop in group['properties']:
                extra = f' index={prop["index"]}' if 'index' in prop else ''
                lines.append(f'{prefix}    {prop["name"]} = {prop["value"]!r}{extra} @0x{prop["offset"]:x} raw={prop["raw"]}')
        for child in obj['children']:
            walk(child, depth+1)
        for frame in obj['named_frames']:
            lines.append(f'{prefix}  Frame {frame["name"]!r} #{frame["frame"]} command={frame["command"]} target={frame["target"]!r} @0x{frame["offset"]:x}')
        for timeline in obj['timelines']:
            lines.append(f'{prefix}  Timeline {timeline["target"]!r}: {len(timeline["keyframes"])} keys @0x{timeline["offset"]:x}')
            for key in timeline['keyframes']:
                values = ', '.join(f'{p["name"]}={p["value"]!r} (raw={p["raw"]})' for p in key['properties'])
                lines.append(f'{prefix}    #{key["frame"]} interpolation/ease={key["interpolation"]}: {values}')
    if scene.root:
        walk(scene.root)
    for error in scene.errors:
        lines.append('ERROR: ' + error)
    if scene.errors:
        lines.append('Raw DATA: ' + scene.section('DATA')['raw'])
    for s in scene.sections:
        if s['tag'] not in ('STRN','VECT','QUAT','DATA'):
            lines.append(f'Raw {s["tag"]}: {s["raw"]}')
    return '\n'.join(lines) + '\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('command', choices=['dump','json','roundtrip'])
    p.add_argument('file', type=Path)
    p.add_argument('--strings', type=Path, help='companion .xus property overrides for dump/json')
    args = p.parse_args()
    try:
        data = args.file.read_bytes()
        scene = parse(data)
        overrides = []
        if args.strings:
            try:
                from . import xus
            except ImportError:
                import xus
            overrides = xus.read(args.strings)
        if args.command == 'dump':
            print(dump(scene), end='')
            if args.strings:
                print(f'Companion localization: {args.strings}')
                for override in overrides:
                    print(f'  {override["path"]} = {override["value"]!r} @XUS+0x{override["offset"]:x}')
        elif args.command == 'json':
            result = scene.as_dict()
            if args.strings:
                result['localization'] = overrides
            print(json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False))
        else:
            if scene.serialize() != data:
                raise FormatError('reserialized bytes differ')
            if not scene.fully_parsed:
                print('Opaque preservation only: ' + '; '.join(scene.errors), file=sys.stderr)
                return 2
            print(f'OK: known sections and DATA records reserialized, {len(data)} bytes, byte-identical')
        return 0 if scene.fully_parsed else 2
    except (OSError, ValueError) as exc:
        print(f'{args.file}: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
