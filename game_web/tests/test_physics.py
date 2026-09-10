import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from bridge import decode, Scene, snapshot
from fixtures import encode, QUAD

class PhysicsTests(unittest.TestCase):
    def test_metadata_and_removal(self):
        scene = Scene()
        scene.apply(decode(encode(objects=[{'id':1,'mesh':QUAD,'rigidBody':{'isDynamic':True,'mass':2.5}}])[4:]))
        self.assertEqual(scene.view()['objects'][0]['rigidBody'], {'isDynamic':True,'mass':2.5})
        scene.apply(decode(encode(objects=[{'id':1}])[4:]))
        self.assertIsNone(scene.view()['objects'][0]['rigidBody'])
        self.assertIn('mesh', scene.view()['objects'][0])

    def test_invalid_mass_is_transactional(self):
        for mass in [float('nan'),float('inf')]:
            with self.assertRaises(ValueError):
                snapshot(encode(objects=[{'id':1,'rigidBody':{'isDynamic':True,'mass':mass}}]))
        # Finite but nonpositive mass reaches the adapter's per-object error path.
        self.assertEqual(snapshot(encode(objects=[{'id':1,'rigidBody':{'isDynamic':True,'mass':0}}]))['objects'][0]['rigidBody']['mass'], 0)

    def test_generation_only_changes_for_new_scene(self):
        scene = Scene(); scene.HISTORY_BATCHES = 1
        start = scene.view()['generation']
        scene.connection(True)
        self.assertEqual(scene.view()['generation'], start+1)
        for _ in range(3): scene.apply(decode(encode()[4:]))
        self.assertEqual(scene.view(0)['kind'], 'full')
        self.assertEqual(scene.view(0)['generation'], start+1)
        scene.connection(False)
        self.assertEqual(scene.view()['generation'], start+1)
        scene.apply(decode(encode(reset=True)[4:]))
        self.assertEqual(scene.view()['generation'], start+2)
