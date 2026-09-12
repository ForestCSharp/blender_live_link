import * as THREE from 'three';
import { Gameplay } from './gameplay.js';
import { ScenePhysics } from './physics.js';
import { SceneLights, configureParityShader } from './lighting.js';

const FALLBACK = { color: [0.6,0.6,0.6,1], metallic: 0, roughness: 0.5, emission: [0,0,0,1], emissionStrength: 0 };
const IMAGE_FIELDS = ['colorImage','metallicImage','roughnessImage','emissionImage'];
export class SceneResources {
  constructor(scene) {
    this.scene = scene;
    this.physics = new ScenePhysics();
    this.physicsSource = null;
    this.gameplay = new Gameplay(this);
    this.group = new THREE.Group();
    scene.add(this.group);
    this.objects = new Map(); this.materialData = new Map(); this.imageData = new Map();
    this.meshes = new Map(); this.materials = new Map(); this.textures = new Map();
    this.lights = new SceneLights(scene);
    this.dirtyMeshes = new Set();
    this.fullSnapshots = 0; this.incrementalBatches = 0; this.geometryUploads = 0;
    this.disposed = false; this.missingImages = false; this.imageRetryAt = 0;
    this.abort = new AbortController();
  }
  clear() {
    this.gameplay.clear();
    for (const mesh of this.meshes.values()) { mesh.skeleton?.dispose(); mesh.geometry.dispose(); this.group.remove(mesh); }
    for (const material of this.materials.values()) material.dispose();
    for (const record of this.textures.values()) record.texture.dispose();
    this.objects.clear(); this.meshes.clear(); this.materialData.clear(); this.materials.clear();
    this.imageData.clear(); this.textures.clear(); this.dirtyMeshes.clear();
    for (const id of [...this.lights.items.keys()]) this.lights.remove(id);
  }
  async apply(data) {
    if (data.kind === 'full') {
      const key = `${data.session}:${data.generation ?? 0}`;
      if (this.physicsSource !== key) {
        this.clear(); this.physics.dispose(); this.physics = new ScenePhysics();
        this.physicsSource = key;
      } else {
        // Recovery snapshots reconcile existing GPU and physics resources.
        const ids = new Set(data.objects.map(o => o.id));
        this.accumulate({ deleted: [...this.objects.keys()].filter(id => !ids.has(id)), objects: [], materials: [] });
        this.materialData.clear(); this.imageData.clear();
      }
      this.fullSnapshots++;
      this.accumulate(data);
    } else if (data.kind === 'delta') {
      for (const batch of data.batches) { this.accumulate(batch); this.incrementalBatches++; }
    } else return;
    if (data.activeCameraControlId !== undefined) this.gameplay.activeCameraId = data.activeCameraControlId;
    await this.physics.ready;
    if (this.disposed) return;
    await this.refreshTextures();
    if (!this.disposed) this.sync();
  }
  accumulate(batch) {
    for (const id of batch.deleted || []) {
      this.objects.delete(id);
      const old = this.meshes.get(id);
      if (old) { old.skeleton?.dispose(); old.geometry.dispose(); this.group.remove(old); this.meshes.delete(id); }
    }
    for (const object of batch.objects) {
      const previous = this.objects.get(object.id);
      if (object.mesh !== undefined && JSON.stringify(previous?.mesh) !== JSON.stringify(object.mesh)) this.dirtyMeshes.add(object.id);
      this.objects.set(object.id, { ...previous, ...object });
    }
    for (const material of batch.materials) this.materialData.set(material.id, material);
    for (const image of batch.images || []) this.imageData.set(image.id, image);
  }
  usedMaterials() {
    const result = new Set();
    for (const object of this.objects.values()) if (object.mesh) result.add(object.mesh.materialIds[0] ?? -1);
    return result;
  }
  async refreshTextures() {
    const used = new Set();
    for (const id of this.usedMaterials()) {
      const material = this.materialData.get(id);
      if (material) for (const field of IMAGE_FIELDS) if (this.imageData.has(material[field])) used.add(material[field]);
    }
    this.missingImages = false;
    await Promise.all([...used].map(async id => {
      const image = this.imageData.get(id);
      if (this.textures.get(id)?.version === image.version) return;
      try {
        const response = await fetch(image.url, { signal: this.abort.signal });
        if (!response.ok) throw new Error(`Image ${id}: HTTP ${response.status}`);
        const bytes = new Uint8Array(await response.arrayBuffer());
        if (bytes.length !== image.width * image.height * 4) throw new Error(`Image ${id}: invalid RGBA bytes`);
        if (this.disposed || this.imageData.get(id) !== image) return;
        const texture = new THREE.DataTexture(bytes, image.width, image.height, THREE.RGBAFormat);
        texture.colorSpace = THREE.LinearSRGBColorSpace;
        texture.flipY = false; // Blender row zero and UV v=0 are both the bottom row.
        texture.wrapS = texture.wrapT = THREE.RepeatWrapping;
        texture.magFilter = THREE.LinearFilter;
        texture.minFilter = THREE.LinearMipmapLinearFilter;
        texture.generateMipmaps = true;
        texture.needsUpdate = true;
        this.textures.get(id)?.texture.dispose();
        this.textures.set(id, { version: image.version, texture });
      } catch (error) {
        if (!this.disposed) {
          this.textures.get(id)?.texture.dispose(); this.textures.delete(id);
          this.missingImages = true; this.lastImageError = error.message;
        }
      }
    }));
    for (const [id, record] of this.textures) if (!used.has(id)) { record.texture.dispose(); this.textures.delete(id); }
    this.imageRetryAt = performance.now() + 2000;
  }
  updateMaterial(id) {
    const data = this.materialData.get(id) || FALLBACK;
    const emissive = data.emissionStrength > 0;
    let material = this.materials.get(id);
    if (!material || Boolean(material.isMeshBasicMaterial) !== emissive) {
      material?.dispose();
      material = emissive ? new THREE.MeshBasicMaterial() : new THREE.MeshStandardMaterial();
      material.side = THREE.DoubleSide;
      if (!emissive) configureParityShader(material);
      this.materials.set(id, material);
    }
    const texture = field => this.textures.get(data[field])?.texture || null;
    const map = texture(emissive ? 'emissionImage' : 'colorImage');
    const color = emissive ? data.emission : data.color;
    material.color.setRGB(...(map ? [1,1,1] : color.slice(0,3)), THREE.LinearSRGBColorSpace);
    if (emissive) material.color.multiplyScalar(data.emissionStrength);
    const previousMap = material.map;
    material.map = map;
    material.opacity = emissive || map ? 1 : THREE.MathUtils.clamp(color[3], 0, 1);
    // Native opaque geometry does not alpha blend; preserve alpha for inspection only.
    material.transparent = false;
    if (!emissive) {
      const rough = texture('roughnessImage'), metal = texture('metallicImage');
      if (material.roughnessMap !== rough || material.metalnessMap !== metal) material.needsUpdate = true;
      material.roughnessMap = rough; material.metalnessMap = metal;
      material.roughness = rough ? 1 : THREE.MathUtils.clamp(data.roughness, 0, 1);
      material.metalness = metal ? 1 : THREE.MathUtils.clamp(data.metallic, 0, 1);
    }
    if (previousMap !== map) material.needsUpdate = true;
    return material;
  }
  sync() {
    const used = this.usedMaterials();
    for (const id of used) this.updateMaterial(id);
    for (const [id, material] of this.materials) if (!used.has(id)) { material.dispose(); this.materials.delete(id); }
    for (const [id, object] of this.objects) {
      const data = object.mesh;
      if (!data) { const old=this.meshes.get(id); if(old){old.skeleton?.dispose();old.geometry.dispose();old.removeFromParent();this.meshes.delete(id);} continue; }
      let mesh = this.meshes.get(id);
      if (!mesh || this.dirtyMeshes.has(id)) {
        const geometry = new THREE.BufferGeometry();
        geometry.setAttribute('position', new THREE.Float32BufferAttribute(data.positions, 3));
        geometry.setIndex(data.indices);
        if (data.normals.length) geometry.setAttribute('normal', new THREE.Float32BufferAttribute(data.normals, 3));
        else geometry.computeVertexNormals();
        if (data.uvs?.length) geometry.setAttribute('uv', new THREE.Float32BufferAttribute(data.uvs, 2));
        // Materials with maps also need UVs on meshes lacking authored UVs.
        else geometry.setAttribute('uv', new THREE.Float32BufferAttribute(new Float32Array(data.positions.length / 3 * 2), 2));
        const skinned = !!data.jointIndices?.length;
        if (skinned) {
          // Bone zero is an identity sentinel for native zero-weight vertices.
          const joints = data.jointIndices.map(i=>i+1), weights = [...data.jointWeights];
          for(let i=0;i<weights.length;i+=4) if(weights.slice(i,i+4).reduce((a,b)=>a+b,0)<=1e-5) {
            joints[i]=0; weights.splice(i,4,1,0,0,0);
          }
          geometry.setAttribute('skinIndex', new THREE.Float32BufferAttribute(joints,4));
          geometry.setAttribute('skinWeight', new THREE.Float32BufferAttribute(weights,4));
        }
        if(mesh && !!mesh.isSkinnedMesh !== skinned) {
          mesh.skeleton?.dispose(); mesh.geometry.dispose(); mesh.removeFromParent(); mesh=null;
        }
        if (mesh) { mesh.geometry.dispose(); mesh.geometry = geometry; }
        else { mesh = skinned ? new THREE.SkinnedMesh(geometry) : new THREE.Mesh(geometry); this.group.add(mesh); this.meshes.set(id, mesh); }
        this.geometryUploads++;
      }
      mesh.material = this.materials.get(data.materialIds[0] ?? -1);
      mesh.position.fromArray(object.position);
      mesh.quaternion.fromArray(object.rotation).normalize();
      mesh.scale.fromArray(object.scale);
      mesh.visible = object.visible && !object.part && !object.attachment;
      mesh.name = object.name;
      mesh.castShadow = mesh.receiveShadow = true;
    }
    this.physics.reconcile(this.objects);
    this.physics.write(this.meshes);
    this.gameplay.reconcile();
    this.group.updateMatrixWorld(true);
    // Bounds also change on transform-only deltas and deletion.
    this.lights.sync(this.gameplay.lightObjects, this.bounds(), true);
    this.dirtyMeshes.clear();
  }
  stepPhysics(dt, hidden = false) {
    const moved = this.physics.step(dt, this.meshes, hidden, step=>this.gameplay.beforeStep(step), step=>this.gameplay.advance(step));
    if (moved || this.gameplay.changed || (this.physics.lastSteps && (this.gameplay.mechs.size || this.gameplay.playbacks.size))) {
      this.gameplay.update(); this.lights.sync(this.gameplay.lightObjects, this.bounds(), true); this.gameplay.changed=false;
    }
  }
  resetPhysics() { this.physics.reset(this.objects); this.gameplay.rewind(); this.sync(); }
  bounds() {
    const box = new THREE.Box3();
    this.group.updateMatrixWorld(true);
    for (const mesh of [...this.meshes.values(), ...this.gameplay.runtimeMeshes()]) if (mesh.visible) box.expandByObject(mesh);
    return box;
  }
  diagnostics() {
    let meshCount = 0, triangles = 0;
    for (const mesh of [...this.meshes.values(), ...this.gameplay.runtimeMeshes()]) if (mesh.visible) { meshCount++; triangles += mesh.geometry.index.count / 3; }
    return { ...this.gameplay.diagnostics(), ...this.physics.diagnostics(), meshCount, triangles, fullSnapshots: this.fullSnapshots, incrementalBatches: this.incrementalBatches,
      geometryUploads: this.geometryUploads, materials: this.materials.size, textures: this.textures.size,
      lights: this.lights.items.size, previewLights: this.lights.preview.visible,
      objects: [...this.meshes].map(([id, mesh]) => ({ id, geometry: mesh.geometry.uuid, position: mesh.position.toArray(), scale: mesh.scale.toArray(), visible: mesh.visible })) };
  }
  dispose() { this.disposed = true; this.abort.abort(); this.physics.dispose(); this.clear(); this.lights.dispose(); this.scene.remove(this.group); }
}
