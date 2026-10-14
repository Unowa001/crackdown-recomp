"""Checked semantic edits for fully decoded XUI v5 scenes and XUS overrides.

Tables are append-only: changing a property never mutates shared strings or
vectors. Call finish() for strict serialization/reparse at the file boundary.
"""
from copy import deepcopy
import struct

from . import xur, xus
from .schema import CLASSES, hierarchy

REFERENCE_PROPERTIES = {
    'DefaultFocus', 'NavLeft', 'NavRight', 'NavUp', 'NavDown',
    'PressAnimObject', 'FocusAnimObject', 'TextureSurfaceElement',
}


def find(root, path):
    """Find an exact Id path, optionally including the root Id.

    An anonymous canvas is omitted. Each step is a direct child lookup; missing
    or ambiguous siblings fail rather than choosing a same-named descendant.
    """
    parts = path.split('/')
    if not path or any(not p for p in parts):
        raise ValueError('expected a nonempty scoped Id path')
    if root['id'] and parts[0] == root['id']:
        parts = parts[1:]
    obj = root
    for part in parts:
        matches = [c for c in obj['children'] if c['id'] == part]
        if len(matches) != 1:
            raise ValueError(f'missing or ambiguous path {path!r} at {part!r}')
        obj = matches[0]
    return obj


def property_value(obj, name):
    return next(p['value'] for p in obj['properties'] if p['name'] == name)


def rebuild_groups(obj):
    """Rebuild base-first masks/descriptors and flattened property references."""
    by_name = {p['name']: p for p in obj['properties']}
    groups = []
    for cls in reversed(hierarchy(obj['class'])):
        fields = CLASSES[cls][1]
        props = [by_name[name] for name, _ in fields if name in by_name]
        mask = sum(1 << i for i, (name, _) in enumerate(fields) if name in by_name)
        width = (mask.bit_length() + 7) // 8
        # Guest 0x826FCC50 branches only for mask widths 1, 2 and 4.
        # A 3-byte mask is skipped without consuming its property values,
        # then 0x826FDCC8 rejects the count with HRESULT 0x8030000D.
        if width == 3:
            width = 4
        if len(props) > 32 or width > 4:
            raise ValueError('property descriptor exceeds wire limits')
        groups.append(dict(class_name=cls, packed=((len(props)-1) << 3 | width) if props else 0,
                           mask=mask.to_bytes(width, 'big').hex(), properties=props))
    if sum(len(g['properties']) for g in groups) != len(by_name):
        raise ValueError('property does not belong to element class')
    obj['property_groups'] = groups
    obj['properties'] = [p for g in groups for p in g['properties']]
    obj['property_count'] = len(obj['properties'])
    obj['flags'] = (obj['flags'] & ~1) | bool(obj['properties'])
    obj['id'] = by_name.get('Id', {}).get('value', '')


class Editor:
    def __init__(self, scene):
        if not scene.fully_parsed:
            raise ValueError(f'refusing incomplete scene: {scene.errors}')
        self.scene = scene

    @classmethod
    def parse(cls, data):
        return cls(xur.parse(data))

    def find(self, path):
        return find(self.scene.root, path)

    def intern(self, value):
        for item in self.scene.strings:
            if item['value'] == value:
                return item['index']
        if len(self.scene.strings) >= 65535:
            raise ValueError('string index limit reached')
        if len(value.encode('utf-16-be')) // 2 > 65535:
            raise ValueError('string length limit reached')
        index = len(self.scene.strings)
        self.scene.strings.append(dict(index=index, offset=None, value=value))
        return index

    def make_property(self, name, kind, value):
        prop = dict(name=name, type=kind, value=deepcopy(value), raw='')
        if kind == 's':
            prop['index'] = self.intern(value)
        elif kind in ('v', 'q'):
            width, tag = (3, 'VECT') if kind == 'v' else (4, 'QUAT')
            raw = struct.pack('>' + 'f'*width, *value)
            table = self.scene.vectors if kind == 'v' else self.scene.quaternions
            index = next((i for i, p in enumerate(table) if p['raw'] == raw.hex()), len(table))
            if index == len(table):
                table.append(dict(index=index, offset=None, value=list(struct.unpack('>'+'f'*width, raw)), raw=raw.hex()))
            prop.update(index=index, value=deepcopy(table[index]['value']))
            if not self.scene.section(tag, False):
                self.scene.sections.insert(-1, dict(tag=tag, offset=0, size=0, raw='', gap_before=''))
        elif kind.startswith('a'):
            if len(value) > 255:
                raise ValueError('array count limit reached')
            prop['items'] = [self.make_property(name, kind[1:], v) for v in value]
        elif kind == 'o':
            cls = 'XuiFigureFillGradient' if name == 'Gradient' else 'XuiFigure' + name
            fields = CLASSES[cls][1]
            if set(value) - {n for n, _ in fields}:
                raise ValueError('unknown compound field')
            prop['items'] = [self.make_property(n, k, value[n]) for n, k in fields if n in value]
            mask = sum(1 << i for i, (n, _) in enumerate(fields) if n in value)
            prop.update(count=sum(len(p['items']) if p['type'].startswith('a') else 1 for p in prop['items']),
                        packed=0, mask=mask.to_bytes(max(1, (len(fields)+7)//8), 'big').hex())
        elif kind == 'r':
            # Geometry allocations/offset relocation are intentionally not inferred.
            if value not in self.scene.custom:
                raise ValueError('CUST value must be an existing scene record')
            prop['index'] = value['relative_offset']
        elif kind == 'b':
            if not isinstance(value, bool):
                raise ValueError('boolean property requires bool')
        elif kind == 'c':
            if len(bytes.fromhex(value.removeprefix('#'))) != 4:
                raise ValueError('color requires four ARGB bytes')
        elif kind == 'f':
            prop['raw'] = struct.pack('>f', value).hex()
        # Validate scalar ranges and all encoded values before changing the node.
        xur.encode_property(prop)
        return prop

    def set_property(self, obj, name, value):
        kinds = {n: k for cls in hierarchy(obj['class']) for n, k in CLASSES[cls][1]}
        if name not in kinds:
            raise ValueError(f'{obj["class"]} has no property {name!r}')
        prop = self.make_property(name, kinds[name], value)
        obj['properties'] = [p for p in obj['properties'] if p['name'] != name] + [prop]
        rebuild_groups(obj)
        return prop

    def clone(self, source, parent, new_id, child_index=None):
        """Deep clone within this scene, retaining child Ids in their local scope.

        Self references in the subtree (properties, named frames, timelines) are
        retargeted. Animations owned by ancestors require clone_timelines().
        """
        if not new_id or any(c['id'] == new_id for c in parent['children']):
            raise ValueError('clone Id must be nonempty and unique among siblings')
        index = len(parent['children']) if child_index is None else child_index
        if not 0 <= index <= len(parent['children']):
            raise ValueError('child index out of bounds')
        if not any(n is source for n in xur.nodes(self.scene.root)) or not any(n is parent for n in xur.nodes(self.scene.root)):
            raise ValueError('clone source and parent must belong to this scene')
        clone = deepcopy(source)
        old_id = source['id']
        self.set_property(clone, 'Id', new_id)
        for obj in xur.nodes(clone):
            for p in list(obj['properties']):
                if old_id and p['type'] == 's' and p['name'] in REFERENCE_PROPERTIES and p['value'] == old_id:
                    self.set_property(obj, p['name'], new_id)
            for frame in obj['named_frames']:
                if old_id and frame['target'] == old_id:
                    frame.update(target=new_id, target_index=self.intern(new_id))
            for timeline in obj['timelines']:
                if old_id and timeline['target'] == old_id:
                    timeline.update(target=new_id, target_index=self.intern(new_id))
                for key in timeline['keyframes']:
                    for i, p in enumerate(key['properties']):
                        if old_id and p['type'] == 's' and p['name'] in REFERENCE_PROPERTIES and p['value'] == old_id:
                            key['properties'][i] = self.make_property(p['name'], 's', new_id)
        parent['children'].insert(index, clone)
        parent['flags'] |= 2
        return clone

    def clone_timelines(self, owner, source, clone):
        """Clone all owner animations for source, checking unambiguous targets."""
        for obj in (source, clone):
            matches = [n for n in xur.nodes(owner) if n['id'] == obj['id']]
            if len(matches) != 1 or matches[0] is not obj:
                raise ValueError('timeline target is missing or ambiguous in owner scope')
        if source['class'] != clone['class']:
            raise ValueError('timeline clone requires the same class hierarchy')
        timelines = [deepcopy(t) for t in owner['timelines'] if t['target'] == source['id']]
        if not timelines:
            raise ValueError('source has no owner timelines')
        for t in timelines:
            t.update(target=clone['id'], target_index=self.intern(clone['id']))
            for key in t['keyframes']:
                for i, p in enumerate(key['properties']):
                    if p['type'] == 's' and p['name'] in REFERENCE_PROPERTIES and p['value'] == source['id']:
                        key['properties'][i] = self.make_property(p['name'], 's', clone['id'])
        owner['timelines'].extend(timelines)
        owner['flags'] |= 4
        return timelines

    def finish(self):
        data = self.scene.serialize()
        parsed = xur.parse(data)
        if not parsed.fully_parsed or parsed.serialize() != data:
            raise ValueError(f'edited scene failed strict reparse: {parsed.errors}')
        return data


def set_localized(records, path, text):
    """Add or replace exactly one property override; refuse ambiguous duplicates."""
    matches = [r for r in records if r['path'] == path]
    if len(matches) > 1:
        raise ValueError(f'duplicate localization path: {path}')
    if matches:
        matches[0]['value'] = text
    else:
        records.append(dict(path=path, value=text))
    data = xus.build(records)
    if xus.build(xus.parse(data)) != data:
        raise ValueError('edited localization failed strict reparse')
    return data
