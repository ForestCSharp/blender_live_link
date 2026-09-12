import * as THREE from 'three';
import { Playback, matrix, updateSkin } from './animation.js';

export const PART_NAMES = ['Body', 'Legs', 'Left Arm', 'Right Arm', 'Head'];
const UP = new THREE.Vector3(0,0,1);
const forwardOf = q => new THREE.Vector3(0,1,0).applyQuaternion(q);
export function turnHeading(rotation, direction, dt) {
  if (direction.x * direction.x + direction.y * direction.y <= 1e-6) return;
  rotation.slerp(new THREE.Quaternion().setFromAxisAngle(UP, Math.atan2(-direction.x, direction.y)), Math.min(10 * dt, 1));
}
export function characterVelocity(current, direction, settings, jump, dt) {
  const blend = Math.min(15 * dt, 1);
  return [current[0] * (1-blend) + settings.moveSpeed * direction.x * blend,
    current[1] * (1-blend) + settings.moveSpeed * direction.y * blend,
    current[2] + (jump ? settings.jumpSpeed : 0)];
}

export class Gameplay {
  constructor(resources) {
    this.resources = resources;
    this.mechs = new Map(); this.optOut = new Set(); this.playbacks = new Map();
    this.playing = true; this.rate = 1; this.playerId = null; this.activeCameraId = null;
    this.input = null; this.serial = 0; this.epoch = 0; this.errors = []; this.lightObjects = new Map();
  }
  clear() {
    this.epoch++;
    for (const mech of this.mechs.values()) this.destroyParts(mech);
    this.mechs.clear(); this.optOut.clear(); this.playbacks.clear();
    this.playerId = this.activeCameraId = null; this.playing = true; this.rate = 1; this.serial++;
  }
  destroyParts(mech) {
    for (const part of mech.parts.values()) {
      part.mesh?.skeleton?.dispose(); part.mesh?.removeFromParent();
    }
    mech.parts.clear();
  }
  create(id) {
    if (!this.resources.objects.get(id)?.character || this.mechs.has(id)) return;
    this.optOut.delete(id);
    this.mechs.set(id, { id, loadout:Array(5).fill(null), parts:new Map(), armatures:new Map(), errors:[] });
    this.reconcile();
  }
  remove(id) {
    const mech = this.mechs.get(id);
    if (mech) this.destroyParts(mech);
    this.mechs.delete(id); this.optOut.add(id); this.serial++;
    this.update();
  }
  select(id, slot, template) {
    const mech = this.mechs.get(id);
    if (!mech) return;
    mech.loadout[slot] = template; this.reconcile();
  }
  playback(map, id) {
    const data = this.resources.objects.get(id)?.armature;
    if (!data) { map.delete(id); return null; }
    let record = map.get(id);
    if (!record) { record = new Playback(data); map.set(id, record); }
    else if (record.data !== data && JSON.stringify(record.data) !== JSON.stringify(data)) {
      const name = record.data.animations[record.clip]?.name;
      record.data = data;
      record.clip = Math.max(0, data.animations.findIndex(c => c.name === name));
      record.advance(0);
    }
    return record;
  }
  reconcile() {
    const { objects, meshes, physics } = this.resources;
    const characters = [...objects.values()].filter(o => o.character).sort((a,b)=>a.id-b.id);
    this.playerId = characters.find(o => o.character.playerControlled)?.id ?? null;
    for (const id of [...this.mechs.keys()]) if (!objects.get(id)?.character) {
      this.destroyParts(this.mechs.get(id)); this.mechs.delete(id);
    }
    for (const id of this.optOut) if (!objects.get(id)?.character) this.optOut.delete(id);
    for (const object of characters) {
      const record = physics.bodies.get(object.id);
      if (record && !record.torso) {
        record.torso = new THREE.Quaternion().fromArray(object.rotation).normalize();
        record.legs = record.torso.clone();
      }
      if (!this.mechs.has(object.id) && !this.optOut.has(object.id))
        this.mechs.set(object.id, {id:object.id,loadout:Array(5).fill(null),parts:new Map(),armatures:new Map(),errors:[]});
    }
    const templates = [...objects.values()].filter(o=>o.part).sort((a,b)=>a.id-b.id);
    const usedOrdinary = new Set();
    for (const object of objects.values()) if (object.mesh?.jointIndices?.length && !object.part && !object.attachment) {
      usedOrdinary.add(object.mesh.armatureId); this.playback(this.playbacks, object.mesh.armatureId);
    }
    for (const id of this.playbacks.keys()) if (!usedOrdinary.has(id)) this.playbacks.delete(id);
    for (const mech of this.mechs.values()) {
      const used = new Set();
      for (let slot=0; slot<5; slot++) {
        const template = mech.loadout[slot] === null ? templates.find(t=>t.part.type===slot)
          : objects.get(mech.loadout[slot]);
        const valid = template?.part?.type === slot;
        let part = mech.parts.get(slot);
        const sourceMesh = valid ? meshes.get(template.id) : null;
        if (part && (!valid || part.templateId !== template.id || part.mesh?.isSkinnedMesh !== sourceMesh?.isSkinnedMesh || !!part.mesh !== !!sourceMesh)) {
          part.mesh?.skeleton?.dispose(); part.mesh?.removeFromParent(); mech.parts.delete(slot); part=null;
        }
        if (!valid) continue;
        if (!part) {
          part = {templateId:template.id, key:`mech:${mech.id}:${slot}`, mesh:null, transform:new THREE.Object3D()};
          if (sourceMesh) {
            part.mesh = sourceMesh.isSkinnedMesh ? new THREE.SkinnedMesh(sourceMesh.geometry, sourceMesh.material)
              : new THREE.Mesh(sourceMesh.geometry, sourceMesh.material);
            part.mesh.name = `${template.name} [Mech ${mech.id}]`;
            part.mesh.castShadow = part.mesh.receiveShadow = true;
            this.resources.group.add(part.mesh);
          }
          mech.parts.set(slot,part);
        }
        if (part.mesh) { part.mesh.geometry = sourceMesh.geometry; part.mesh.material = sourceMesh.material; }
        if (template.mesh?.jointIndices?.length) {
          used.add(template.mesh.armatureId); this.playback(mech.armatures, template.mesh.armatureId);
        }
      }
      for (const id of mech.armatures.keys()) if (!used.has(id)) mech.armatures.delete(id);
    }
    this.serial++; this.update();
  }
  beforeStep(dt) {
    const { physics, objects } = this.resources;
    const record = physics.bodies.get(this.playerId), object = objects.get(this.playerId);
    if (!record?.body || !object?.character || !this.input || this.input.debug) return;
    const { camera, keys } = this.input;
    const forward = camera.getWorldDirection(new THREE.Vector3()); forward.z=0;
    if (forward.lengthSq()<1e-6) forward.set(0,1,0); forward.normalize();
    const right = new THREE.Vector3().crossVectors(forward, UP).normalize();
    const direction = forward.clone().multiplyScalar(Number(keys.has('KeyW'))-Number(keys.has('KeyS')))
      .addScaledVector(right, Number(keys.has('KeyD'))-Number(keys.has('KeyA')));
    if (direction.lengthSq()>1) direction.normalize();
    if (keys.has('ShiftLeft')) direction.multiplyScalar(3);
    const v = record.body.GetLinearVelocity();
    const desired = characterVelocity([v.GetX(),v.GetY(),v.GetZ()], direction, object.character, keys.has('Space'), dt);
    const velocity = new physics.J.Vec3(...desired);
    physics.interface.SetLinearVelocity(record.body.GetID(), velocity); physics.J.destroy(velocity);
    turnHeading(record.torso, forward, dt); turnHeading(record.legs, direction, dt);
  }
  advance(dt) {
    if (!this.playing) return;
    for (const playback of this.playbacks.values()) playback.advance(dt * this.rate);
    for (const mech of this.mechs.values()) for (const playback of mech.armatures.values()) playback.advance(dt * this.rate);
  }
  rewind() {
    for (const p of this.playbacks.values()) p.rewind();
    for (const mech of this.mechs.values()) for (const p of mech.armatures.values()) p.rewind();
    this.update();
  }
  socketMatrix(body, socket, mech) {
    if (!socket.valid) throw new Error('socket was invalid when exported');
    const result = body.transform.matrix.clone();
    if (socket.bindingType === 1) {
      const data = this.resources.objects.get(body.templateId).mesh;
      if (!data?.jointIndices?.length || socket.armatureId !== data.armatureId) throw new Error('socket armature does not match Body');
      const playback = mech.armatures.get(socket.armatureId);
      if (!playback) throw new Error('socket armature is missing');
      const index = playback.data.bones.findIndex(b=>b.name===socket.boneName);
      if (index<0) throw new Error('socket bone is missing');
      result.multiply(matrix(data.armatureToMesh)).multiply(playback.pose(index));
    }
    return result.multiply(matrix(socket.localTransform));
  }
  update() {
    this.changed = true;
    const { objects, physics, meshes } = this.resources;
    this.errors = []; this.lightObjects.clear();
    for (const object of objects.values()) {
      if (object.part || object.attachment) continue;
      const mesh = meshes.get(object.id);
      if (mesh?.isSkinnedMesh) {
        const error = updateSkin(mesh, object.mesh, this.playbacks.get(object.mesh.armatureId));
        mesh.visible = object.visible && !error;
        if (error) this.errors.push(`${object.name}: ${error}`);
      }
      if (object.light) this.lightObjects.set(object.id, {...object,position:physics.position(object.id,object.position)});
    }
    for (const mech of this.mechs.values()) {
      mech.errors = [];
      const character = objects.get(mech.id), record = physics.bodies.get(mech.id);
      const body = mech.parts.get(0);
      for (let slot=0;slot<5;slot++) {
        const part = mech.parts.get(slot);
        if (!part) { mech.errors.push(`${PART_NAMES[slot]}: ${mech.loadout[slot]===null?'default template is missing':'explicit template is unavailable'}`); continue; }
        if (part.mesh) part.mesh.visible = false;
        try {
          if (!body) throw new Error('Body is missing');
          const template = objects.get(part.templateId), transform = part.transform;
          if (slot===0) {
            transform.position.fromArray(physics.position(mech.id,character.position));
            transform.quaternion.copy(record?.torso || new THREE.Quaternion().fromArray(character.rotation));
          } else {
            const socket = [...objects.values()].filter(o=>o.attachment?.ownerPartId===body.templateId && o.attachment.type===slot).sort((a,b)=>a.id-b.id)[0]?.attachment;
            if (!socket) throw new Error('Body socket is missing');
            const world = this.socketMatrix(body,socket,mech);
            // Native extracts rotation from independently normalized matrix columns.
            const e=world.elements;
            const rotation=world.clone();
            for(let c=0;c<3;c++) {
              const length=Math.hypot(e[c*4],e[c*4+1],e[c*4+2]);
              if(length<=Math.sqrt(1e-7))throw new Error('socket transform is singular');
              for(let r=0;r<3;r++)rotation.elements[c*4+r]/=length;
            }
            transform.position.setFromMatrixPosition(world);
            transform.quaternion.setFromRotationMatrix(rotation).normalize();
            if(slot===1)transform.quaternion.copy(record?.legs || new THREE.Quaternion().fromArray(character.rotation));
          }
          transform.scale.fromArray(template.scale); transform.updateMatrix();
          if (part.mesh) {
            part.mesh.position.copy(transform.position); part.mesh.quaternion.copy(transform.quaternion); part.mesh.scale.copy(transform.scale);
            const error=updateSkin(part.mesh,template.mesh,mech.armatures.get(template.mesh.armatureId));
            if(error)throw new Error(error);
            part.mesh.visible=true;
          }
          if(template.light)this.lightObjects.set(part.key,{...template,visible:true,position:transform.position.toArray(),rotation:transform.quaternion.toArray()});
        } catch(error) { mech.errors.push(`${PART_NAMES[slot]}: ${error.message}`); }
      }
    }
  }
  runtimeMeshes() { return [...this.mechs.values()].flatMap(m=>[...m.parts.values()].flatMap(p=>p.mesh?[p.mesh]:[])); }
  diagnostics() {
    return {playerCharacterId:this.playerId,activeCameraControlId:this.activeCameraId,
      mechCount:this.mechs.size,skeletons:[...this.resources.meshes.values(),...this.runtimeMeshes()].filter(m=>m.skeleton).length,
      mechs:[...this.mechs.values()].map(m=>({characterId:m.id,loadout:m.loadout,errors:m.errors,
        parts:[...m.parts].map(([slot,p])=>({slot,templateId:p.templateId,position:p.transform.position.toArray(),rotation:p.transform.quaternion.toArray(),visible:p.mesh?.visible??false}))})),gameplayErrors:this.errors};
  }
}
