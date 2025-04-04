# automatically generated by the FlatBuffers compiler, do not modify

# namespace: LiveLink

import flatbuffers
from flatbuffers.compat import import_numpy
np = import_numpy()

class Update(object):
    __slots__ = ['_tab']

    @classmethod
    def GetRootAs(cls, buf, offset=0):
        n = flatbuffers.encode.Get(flatbuffers.packer.uoffset, buf, offset)
        x = Update()
        x.Init(buf, n + offset)
        return x

    @classmethod
    def GetRootAsUpdate(cls, buf, offset=0):
        """This method is deprecated. Please switch to GetRootAs."""
        return cls.GetRootAs(buf, offset)
    # Update
    def Init(self, buf, pos):
        self._tab = flatbuffers.table.Table(buf, pos)

    # Update
    def Objects(self, j):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(4))
        if o != 0:
            x = self._tab.Vector(o)
            x += flatbuffers.number_types.UOffsetTFlags.py_type(j) * 4
            x = self._tab.Indirect(x)
            from Blender.LiveLink.Object import Object
            obj = Object()
            obj.Init(self._tab.Bytes, x)
            return obj
        return None

    # Update
    def ObjectsLength(self):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(4))
        if o != 0:
            return self._tab.VectorLen(o)
        return 0

    # Update
    def ObjectsIsNone(self):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(4))
        return o == 0

    # Update
    def DeletedObjectUids(self, j):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(6))
        if o != 0:
            a = self._tab.Vector(o)
            return self._tab.Get(flatbuffers.number_types.Int32Flags, a + flatbuffers.number_types.UOffsetTFlags.py_type(j * 4))
        return 0

    # Update
    def DeletedObjectUidsAsNumpy(self):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(6))
        if o != 0:
            return self._tab.GetVectorAsNumpy(flatbuffers.number_types.Int32Flags, o)
        return 0

    # Update
    def DeletedObjectUidsLength(self):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(6))
        if o != 0:
            return self._tab.VectorLen(o)
        return 0

    # Update
    def DeletedObjectUidsIsNone(self):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(6))
        return o == 0

    # Update
    def Reset(self):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(8))
        if o != 0:
            return bool(self._tab.Get(flatbuffers.number_types.BoolFlags, o + self._tab.Pos))
        return False

def UpdateStart(builder):
    builder.StartObject(3)

def Start(builder):
    UpdateStart(builder)

def UpdateAddObjects(builder, objects):
    builder.PrependUOffsetTRelativeSlot(0, flatbuffers.number_types.UOffsetTFlags.py_type(objects), 0)

def AddObjects(builder, objects):
    UpdateAddObjects(builder, objects)

def UpdateStartObjectsVector(builder, numElems):
    return builder.StartVector(4, numElems, 4)

def StartObjectsVector(builder, numElems):
    return UpdateStartObjectsVector(builder, numElems)

def UpdateAddDeletedObjectUids(builder, deletedObjectUids):
    builder.PrependUOffsetTRelativeSlot(1, flatbuffers.number_types.UOffsetTFlags.py_type(deletedObjectUids), 0)

def AddDeletedObjectUids(builder, deletedObjectUids):
    UpdateAddDeletedObjectUids(builder, deletedObjectUids)

def UpdateStartDeletedObjectUidsVector(builder, numElems):
    return builder.StartVector(4, numElems, 4)

def StartDeletedObjectUidsVector(builder, numElems):
    return UpdateStartDeletedObjectUidsVector(builder, numElems)

def UpdateAddReset(builder, reset):
    builder.PrependBoolSlot(2, reset, 0)

def AddReset(builder, reset):
    UpdateAddReset(builder, reset)

def UpdateEnd(builder):
    return builder.EndObject()

def End(builder):
    return UpdateEnd(builder)
