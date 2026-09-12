import * as THREE from 'three';

export const matrix = values => new THREE.Matrix4().fromArray(values);
export const identity = () => new THREE.Matrix4();
export class Playback {
  constructor(data) { this.data = data; this.clip = 0; this.time = 0; this.frame = 0; }
  select(index) { this.clip = index; this.rewind(); }
  rewind() { this.time = 0; this.frame = 0; }
  advance(dt) {
    const clip = this.data.animations[this.clip];
    if (!clip?.frameCount) return;
    this.time += dt;
    const duration = clip.duration || (clip.frameRate > 0 ? clip.frameCount / clip.frameRate : 0);
    if (duration > 0) this.time %= duration;
    this.frame = clip.frameRate > 0 ? Math.min(Math.floor(this.time * clip.frameRate), clip.frameCount - 1) : 0;
  }
  skin(index) {
    const clip = this.data.animations[this.clip];
    return clip?.frameCount && index < clip.boneCount
      ? new THREE.Matrix4().fromArray(clip.matrices, (this.frame * clip.boneCount + index) * 16) : identity();
  }
  pose(index) { return this.skin(index).multiply(matrix(this.data.bones[index].inverseBindMatrix).invert()); }
}

// The wire already contains sampled skin matrices, not local bone TRS tracks.
// Flat bones with identity inverses let Three's regular skin/depth shaders consume
// these matrices unchanged, including nonidentity mesh/armature spaces.
export function updateSkin(mesh, data, playback) {
  if (!mesh.isSkinnedMesh) return '';
  const bones = playback?.data.bones;
  const count = (bones?.length || 0) + 1;
  if (!mesh.skeleton || mesh.skeleton.bones.length !== count) {
    mesh.skeleton?.dispose();
    const skeleton = new THREE.Skeleton(Array.from({length:count}, () => {
      const bone = new THREE.Bone(); bone.matrixAutoUpdate = false; return bone;
    }), Array.from({length:count}, identity));
    mesh.bindMode = THREE.DetachedBindMode;
    mesh.bind(skeleton, identity());
  }
  const clip = playback?.data.animations[playback.clip];
  const toMesh = matrix(data.armatureToMesh), toArmature = matrix(data.meshToArmature);
  mesh.skeleton.bones[0].matrixWorld.identity();
  for (let i = 1; i < count; i++) mesh.skeleton.bones[i].matrixWorld.copy(
    clip?.frameCount && i - 1 < clip.boneCount ? toMesh.clone().multiply(playback.skin(i - 1)).multiply(toArmature) : identity());
  mesh.skeleton.update();
  // Bounds must follow deformation for framing, culling and directional shadows.
  if (playback && data.jointIndices.some(i => i >= count - 1)) return 'joint index exceeds armature bone count';
  if (!playback) return 'part armature is missing';
  mesh.computeBoundingBox();
  mesh.boundingSphere = mesh.boundingBox.getBoundingSphere(mesh.boundingSphere || new THREE.Sphere());
  return '';
}
