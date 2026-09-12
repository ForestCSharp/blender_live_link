import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from bridge import Scene, decode, snapshot
from fixtures import encode, QUAD
I=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]
ARM={'bones':[{'name':'root','inverseBindMatrix':I}], 'animations':[{'name':'move','frameRate':2,'duration':1,'frameCount':2,'boneCount':1,'matrices':I*2}]}

class GameplayTests(unittest.TestCase):
    def test_components_and_removal(self):
        data=snapshot(encode(objects=[{'id':1,'character':{'playerControlled':True},'cameraControl':{'followDistance':10,'followSpeed':5}},
             {'id':2,'part':{'type':0}}, {'id':3,'attachment':{'ownerPartId':2,'type':1,'localTransform':I}}]))
        self.assertEqual(data['objects'][0]['character']['moveSpeed'],20)
        self.assertEqual(data['activeCameraControlId'],1)
        self.assertTrue(data['objects'][2]['attachment']['valid'])
        scene=Scene();scene.apply(decode(encode(objects=[{'id':1,'character':{}}])[4:]))
        scene.apply(decode(encode(objects=[{'id':1}])[4:]))
        self.assertIsNone(scene.view()['objects'][0]['character'])

    def test_animation_assets_and_delta_dedup(self):
        scene=Scene(); wire=encode(objects=[{'id':1,'armature':ARM}])
        scene.apply(decode(wire[4:])); revision=scene.revision
        scene.apply(decode(wire[4:]));self.assertNotIn('armature',scene.view(revision)['batches'][0]['objects'][0])
        self.assertEqual(scene.view()['objects'][0]['armature']['animations'][0]['matrices'],I*2)
        scene.apply(decode(encode(objects=[{'id':1}])[4:]))
        self.assertIsNone(scene.view()['objects'][0]['armature'])

    def test_validation(self):
        for obj in [{'id':1,'character':{'radius':0}}, {'id':1,'cameraControl':{'followDistance':0,'followSpeed':1}},
                    {'id':1,'mesh':{**QUAD,'jointIndices':[0],'jointWeights':[1]}},
                    {'id':1,'armature':{**ARM,'animations':[{**ARM['animations'][0],'matrices':I}]}},
                    {'id':1,'attachment':{'ownerPartId':2,'type':1,'localTransform':[1,2]}}]:
            with self.assertRaises(ValueError):snapshot(encode(objects=[obj]))

    def test_camera_selection_and_deleted_dependencies(self):
        scene=Scene();cam={'followDistance':5,'followSpeed':2}
        scene.apply(decode(encode(objects=[{'id':2,'cameraControl':cam},{'id':1,'cameraControl':cam}])[4:]))
        self.assertEqual(scene.view()['activeCameraControlId'],1)
        scene.apply(decode(encode(objects=[{'id':2,'cameraControl':cam}])[4:]))
        self.assertEqual(scene.view()['activeCameraControlId'],2)
        scene.apply(decode(encode(deleted=[2])[4:]))
        self.assertIsNone(scene.view()['activeCameraControlId'])
