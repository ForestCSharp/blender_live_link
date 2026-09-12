import * as THREE from 'three';

export const DEFAULT_POSE = { position: [2.5, -15, 3], forward: [0, 1, -0.5], up: [0, 0, 1] };
const WORLD_UP = new THREE.Vector3(0, 0, 1);
export class DebugCamera {
  constructor(camera, canvas) {
    this.camera = camera;
    this.canvas = canvas;
    this.keys = new Set();
    this.initialized = false;
    this.initial = DEFAULT_POSE;
    this.speed = 10;
    this.sensitivity = 0.002;
    this.applyPose(DEFAULT_POSE);
    canvas.tabIndex = 0;
    canvas.addEventListener('click', () => {
      canvas.focus();
      const request = canvas.requestPointerLock?.();
      request?.catch(() => {}); // Browser can refuse capture; the next click retries.
    });
    document.addEventListener('mousemove', e => {
      if (document.pointerLockElement === canvas) this.look(e.movementX, e.movementY);
    });
    document.addEventListener('pointerlockchange', () => this.keys.clear());
    window.addEventListener('blur', () => this.keys.clear());
    document.addEventListener('visibilitychange', () => this.keys.clear());
    window.addEventListener('keydown', e => {
      if (e.ctrlKey || e.metaKey || e.altKey) { this.keys.clear(); return; }
      if (document.pointerLockElement !== canvas) return;
      if (['KeyW','KeyA','KeyS','KeyD','ArrowUp','ArrowLeft','ArrowDown','ArrowRight','KeyQ','KeyE','ShiftLeft','ShiftRight','Space'].includes(e.code)) {
        e.preventDefault(); this.keys.add(e.code);
      }
    });
    window.addEventListener('keyup', e => this.keys.delete(e.code));
  }
  seed(pose, force = false) {
    if (this.initialized && !force) return;
    this.initial = structuredClone(pose || DEFAULT_POSE);
    this.initialized = true;
    this.reset();
  }
  applyPose(pose) {
    const forward = new THREE.Vector3().fromArray(pose.forward).normalize();
    this.camera.position.fromArray(pose.position);
    this.camera.up.copy(Math.abs(forward.dot(WORLD_UP)) < 0.999 ? WORLD_UP : new THREE.Vector3().fromArray(pose.up));
    this.camera.lookAt(this.camera.position.clone().add(forward));
  }
  reset() { this.keys.clear(); this.applyPose(this.initial); }
  look(dx, dy) {
    const forward = this.camera.getWorldDirection(new THREE.Vector3());
    // Yaw around world Z; pitch around camera right, without changing position.
    let yaw = Math.atan2(forward.y, forward.x);
    let pitch = Math.asin(THREE.MathUtils.clamp(forward.z, -1, 1));
    yaw -= dx * this.sensitivity;
    pitch = THREE.MathUtils.clamp(pitch - dy * this.sensitivity, -Math.PI / 2 + 0.001, Math.PI / 2 - 0.001);
    forward.set(Math.cos(yaw) * Math.cos(pitch), Math.sin(yaw) * Math.cos(pitch), Math.sin(pitch));
    this.camera.up.copy(WORLD_UP);
    this.camera.lookAt(this.camera.position.clone().add(forward));
  }
  move(dt) {
    dt = Math.min(Math.max(dt, 0), 0.1); // Avoid jumps after a suspended tab.
    const forward = this.camera.getWorldDirection(new THREE.Vector3());
    forward.z = 0;
    if (forward.lengthSq() < 1e-6) forward.set(0, 1, 0);
    forward.normalize();
    const right = new THREE.Vector3().crossVectors(forward, WORLD_UP).normalize();
    const held = (...keys) => keys.some(k => this.keys.has(k));
    const step = this.speed * dt * (held('ShiftLeft','ShiftRight') ? 5 : 1);
    this.camera.position.addScaledVector(forward, step * (Number(held('KeyW','ArrowUp')) - Number(held('KeyS','ArrowDown'))));
    this.camera.position.addScaledVector(right, step * (Number(held('KeyD','ArrowRight')) - Number(held('KeyA','ArrowLeft'))));
    this.camera.position.z += step * (Number(held('KeyE')) - Number(held('KeyQ')));
  }
  frame(box) {
    if (box.isEmpty()) return;
    const center = box.getCenter(new THREE.Vector3());
    const radius = Math.max(box.getSize(new THREE.Vector3()).length() / 2, 0.01);
    const vertical = THREE.MathUtils.degToRad(this.camera.fov / 2);
    const horizontal = Math.atan(Math.tan(vertical) * this.camera.aspect);
    const distance = radius / Math.sin(Math.min(vertical, horizontal)) * 1.15;
    const forward = this.camera.getWorldDirection(new THREE.Vector3());
    this.camera.position.copy(center).addScaledVector(forward, -distance);
    this.camera.near = Math.max(radius / 1000, 0.00001);
    this.camera.far = Math.max(distance * 100, 10000);
    this.camera.updateProjectionMatrix();
  }
}
