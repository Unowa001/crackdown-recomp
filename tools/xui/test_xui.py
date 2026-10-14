"""Synthetic format and safety tests; no game bytes or extracted content."""
import struct
import tempfile
from pathlib import Path
import unittest

from tools.xui import xzp, xur, xus


def scene_blob(strings, tree, vectors=b'', flags=0, counts=()):
    """Construct an independent synthetic wire fixture, not through the writer."""
    strn = b''.join(struct.pack('>H', len(s.encode('utf-16-be'))//2)+s.encode('utf-16-be') for s in strings)
    sections = [(b'STRN', strn)]
    if vectors:
        sections.append((b'VECT', vectors))
    sections.append((b'DATA', tree))
    offset = 20 + 4*len(counts) + 12*len(sections)
    table, body = b'', b''
    for name, payload in sections:
        table += struct.pack('>4sII', name, offset, len(payload))
        offset += len(payload)
        body += payload
    return struct.pack('>4sIIHIH', b'XUIB', 5, flags, 12, offset, len(sections)) + b''.join(struct.pack('>I', n) for n in counts) + table + body


def basic_scene():
    # XuiScene: Element Id/Width/Position, Control Text, Scene DefaultFocus.
    # Properties are base-first. Element mask is big-endian 0x000b.
    tree = (b'\x00\x01\x01\x00\x05' + b'\x12\x00\x0b' +
            b'\x00\x02' + struct.pack('>fI', 320.5, 0) +
            b'\x02\x01\x00\x00\x03' + b'\x01\x01\x00\x02')
    return scene_blob(['XuiScene', 'Fixture', 'Synthetic'], tree, struct.pack('>3f', 12, 34, 0))


class PackageTests(unittest.TestCase):
    def test_order_unicode_empty_duplicates(self):
        entries = [('a\\b.xur', b'example'), ('face\U0001f600', b''), ('a\\b.xur', b'example')]
        blob = xzp.build(entries)
        decoded = xzp.parse(blob)
        self.assertEqual([(e.name, e.data) for e in decoded], entries)
        self.assertEqual(xzp.build(decoded), blob)
        self.assertEqual(xzp.get(decoded, 'face\U0001f600'), b'')
        with self.assertRaises(KeyError):
            xzp.get(decoded, 'a\\b.xur')

    def test_bad_size_table_offset_name_and_truncation(self):
        good = xzp.build([('x', b'ab')])
        for at, value in [(8, 0), (16, 0), (26, 1)]:
            bad = bytearray(good)
            struct.pack_into('>I', bad, at, value)
            with self.subTest(at=at), self.assertRaises(xzp.FormatError):
                xzp.parse(bytes(bad))
        for n in range(len(good)):
            with self.subTest(length=n), self.assertRaises(xzp.FormatError):
                xzp.parse(good[:n])

    def test_names_limit_utf16_units(self):
        with self.assertRaises(xzp.FormatError):
            xzp.build([('\U0001f600'*128, b'')])
        self.assertEqual(len(xzp.parse(xzp.build([('a'*255,b'')]))), 1)

    def test_extract_paths_duplicates_and_existing_outputs(self):
        root = Path(__file__).resolve().parents[2]/'out'/'xui'
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as tmp:
            target = Path(tmp)
            entries = [xzp.Entry('d\\x', b'one'), xzp.Entry('d\\x', b'one')]
            xzp.extract(entries, target)
            xzp.extract(entries, target)
            self.assertEqual((target/'d'/'x').read_bytes(), b'one')
            for name in ('../escape', '/absolute', 'C:\\drive', 'd/../../escape', 'd//x'):
                with self.subTest(name=name), self.assertRaises(xzp.FormatError):
                    xzp.extract([xzp.Entry(name, b'x')], target)
            with self.assertRaises(xzp.FormatError):
                xzp.extract([xzp.Entry('d\\x', b'two')], target)
            self.assertEqual((target/'d'/'x').read_bytes(), b'one')
            with self.assertRaises(xzp.FormatError):
                xzp.extract([xzp.Entry('z',b'a'), xzp.Entry('z',b'b')], target)
            (target/'link').symlink_to('/tmp')
            with self.assertRaises(xzp.FormatError):
                xzp.extract([xzp.Entry('link/escape', b'x')], target)
        with self.assertRaises(xzp.FormatError):
            xzp.output_root('/tmp/forbidden-xui-output')


class SceneTests(unittest.TestCase):
    def test_decoding_and_real_serialization(self):
        original = basic_scene()
        scene = xur.parse(original)
        self.assertTrue(scene.fully_parsed, scene.errors)
        obj = scene.root
        self.assertEqual(obj['class'], 'XuiScene')
        self.assertEqual(obj['id'], 'Fixture')
        props = {p['name']:p for p in obj['properties']}
        self.assertEqual(props['Text']['value'], 'Synthetic')
        self.assertEqual(props['Position']['value'], [12,34,0])
        self.assertEqual(props['Width']['value'], 320.5)
        self.assertEqual(scene.serialize(), original)
        # Prove the writer uses parsed fields, not a cached original blob.
        props['Width']['value'] = 640.25
        scene.strings[3]['value'] = 'Changed literal'
        scene.vectors[0]['value'][0] = 99
        rebuilt = xur.parse(scene.serialize())
        changed = {p['name']:p['value'] for p in rebuilt.root['properties']}
        self.assertEqual(changed['Width'], 640.25)
        self.assertEqual(changed['Text'], 'Changed literal')
        self.assertEqual(changed['Position'], [99,34,0])
        self.assertEqual(rebuilt.root['property_groups'][0]['mask'], '000b')

    def test_leaf_named_frames_have_no_timeline_count(self):
        tree = b'\x00\x01\x04' + struct.pack('>I', 1) + struct.pack('>HIBH',2,10,3,65535)
        original = scene_blob(['XuiCanvas','Marker'],tree)
        scene = xur.parse(original)
        self.assertTrue(scene.fully_parsed, scene.errors)
        self.assertEqual(scene.root['named_frames'][0]['target'], None)
        self.assertEqual(scene.serialize(), original)

    def test_children_timelines_and_derived_first_animation_class(self):
        # Canvas -> Button(Id) with a Width timeline. Animation class index 2
        # means Element in Button, Control, Element (derived-first) hierarchy.
        button = b'\x00\x02\x01\x00\x01\x02\x00\x01\x00\x03\x00\x00'
        tree = b'\x00\x01\x06' + struct.pack('>I',1) + button
        tree += struct.pack('>I',1) + struct.pack('>HIBH',4,0,1,65535)
        tree += struct.pack('>IHI',1,3,1) + b'\x01\x02\x01' + struct.pack('>I',2)
        tree += struct.pack('>I4Bf',0,0,0,0,50,10.0) + struct.pack('>I4Bf',30,1,0,0,50,20.0)
        original = scene_blob(['XuiCanvas','XuiButton','Target','Start'], tree)
        scene = xur.parse(original)
        self.assertTrue(scene.fully_parsed,scene.errors)
        self.assertEqual(scene.root['timelines'][0]['definitions'][0]['name'],'Width')
        self.assertEqual([k['frame'] for k in scene.root['timelines'][0]['keyframes']],[0,30])
        self.assertEqual(scene.serialize(),original)
        scene.root['timelines'][0]['keyframes'][1]['properties'][0]['value']=25
        self.assertEqual(xur.parse(scene.serialize()).root['timelines'][0]['keyframes'][1]['properties'][0]['value'],25)

    def test_unknown_bits_remain_raw_not_guessed(self):
        tree = b'\x00\x01\x01\x00\x01\x04\x80\x00\x00\x00\xde\xad\xbe\xef'
        original = scene_blob(['XuiElement'],tree)
        scene = xur.parse(original)
        self.assertFalse(scene.fully_parsed)
        self.assertIn('unknown property bits',scene.errors[0])
        self.assertIn(tree.hex(),xur.dump(scene))
        self.assertEqual(scene.serialize(),original)

    def test_bad_string_index_count_and_header(self):
        for tree in (b'\x00\x02\x00', b'\x00\x01\x02\xff\xff\xff\xff'):
            scene = xur.parse(scene_blob(['XuiCanvas'],tree))
            self.assertFalse(scene.fully_parsed)
        bad = bytearray(basic_scene())
        bad[7] = 8
        with self.assertRaises(xur.FormatError):
            xur.parse(bytes(bad))
        for n in range(20):
            with self.subTest(length=n), self.assertRaises(xur.FormatError):
                xur.parse(basic_scene()[:n])

    def test_count_header_is_validated(self):
        tree = b'\x00\x01\x00'
        good = scene_blob(['XuiCanvas'],tree,flags=1,counts=[1,0,0,0,0,0,0,0,0,0])
        scene = xur.parse(good)
        self.assertTrue(scene.fully_parsed,scene.errors)
        self.assertEqual(scene.serialize(),good)
        bad=bytearray(good)
        struct.pack_into('>I',bad,20,2)
        self.assertIn('extended counts mismatch',xur.parse(bytes(bad)).errors[0])

    def test_nan_payload_preservation(self):
        prop=dict(type='f',value=float('nan'),raw='7fa12345')
        self.assertEqual(xur.encode_property(prop),bytes.fromhex('7fa12345'))


class LocalizationTests(unittest.TestCase):
    def test_exact_xus_endianness_and_empty_value(self):
        # Hand-authored XUIS fixture: value A, key Z.Text; empty value, key B.
        body = b'\x00\x01\x00A\x00\x06\x00Z\x00.\x00T\x00e\x00x\x00t\x00\x00\x00\x01\x00B'
        original=b'XUIS\x01\x00'+struct.pack('>IH',12+len(body),2)+body
        records=xus.parse(original)
        self.assertEqual([(r['path'],r['value']) for r in records],[('Z.Text','A'),('B','')])
        self.assertEqual(xus.build(records),original)
        with self.assertRaises(xur.FormatError):
            xus.parse(original[:-1])


if __name__ == '__main__':
    unittest.main()
