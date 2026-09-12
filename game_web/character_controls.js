import * as THREE from 'three';
import { DebugCamera } from './camera.js';
export class CharacterControls extends DebugCamera {
  constructor(camera, canvas) {
    super(camera, canvas);
    this.debug = true; this.followCameras = new Map(); this.source = null;
  }
  attach(resources) {
    const source = `${resources.physicsSource}`;
    if (resources !== this.resources || source !== this.source) {
      this.keys.clear(); this.followCameras.clear(); this.debug = true;
      this.resources = resources; this.source = source;
    }
    resources.gameplay.input = this;
  }
  toggle() { this.debug = !this.debug; this.keys.clear(); if (!this.debug) this.follow(); }
  follow() {
    const gameplay = this.resources?.gameplay;
    const object = this.resources?.objects.get(gameplay?.activeCameraId);
    if (!object?.cameraControl) return null;
    const signature = JSON.stringify([object.position,object.rotation,object.cameraControl]);
    let pose = this.followCameras.get(object.id);
    if (!pose || pose.signature !== signature) {
      const forward = new THREE.Vector3(0,1,0).applyQuaternion(new THREE.Quaternion().fromArray(object.rotation).normalize());
      pose = {signature,forward,position:new THREE.Vector3().fromArray(this.resources.physics.position(object.id, object.position))
        .addScaledVector(forward,-object.cameraControl.followDistance)};
      this.followCameras.set(object.id,pose);
    }
    return {pose,object};
  }
  look(dx,dy) {
    if(this.debug || !this.resources) return super.look(dx,dy);
    const active=this.follow();
    if(!active)return; // Native has no player orbit without a CameraControl.
    const {pose,object}=active, distance=object.cameraControl.followDistance;
    const target=pose.position.clone().addScaledVector(pose.forward,distance);
    const right=new THREE.Vector3().crossVectors(pose.forward,new THREE.Vector3(0,0,1)).normalize();
    pose.forward.applyAxisAngle(new THREE.Vector3(0,0,1),-dx*this.sensitivity).applyAxisAngle(right,-dy*this.sensitivity).normalize();
    pose.position.copy(target).addScaledVector(pose.forward,-distance);
    this.show(pose);
  }
  show(pose) {
    this.camera.position.copy(pose.position); this.camera.up.set(0,0,1);
    this.camera.lookAt(pose.position.clone().add(pose.forward));
  }
  move(dt) {
    if(this.debug)return super.move(dt);
    const active=this.follow(); if(!active)return;
    const {pose,object}=active;
    const target=new THREE.Vector3().fromArray(this.resources.physics.position(object.id,object.position))
      .addScaledVector(pose.forward,-object.cameraControl.followDistance);
    pose.position.lerp(target,Math.min(object.cameraControl.followSpeed*Math.min(dt,.1),1));
    this.show(pose);
  }
}
