import * as THREE from 'three';

// game/data/shaders/lighting.frag and bruneton_atmosphere.h, atmosphere disabled.
export function lightParameters(data) {
  const power = Math.max(0, data.power);
  const color = new THREE.Color().setRGB(...data.color, THREE.LinearSRGBColorSpace);
  if (data.type === 2) {
    color.multiply(new THREE.Color().setRGB(1.474 * 98242.786222e-5,
      1.8504 * 69954.398112e-5, 1.91198 * 66475.012354e-5));
    return { color, intensity: power / 1361 };
  }
  return { color, intensity: power / (4 * Math.PI) };
}

export function configureParityShader(material) {
  material.onBeforeCompile = shader => {
    // Native material images replace scalars and read red for both scalar maps.
    shader.fragmentShader = shader.fragmentShader
      .replace('#include <roughnessmap_fragment>', THREE.ShaderChunk.roughnessmap_fragment.replace('texelRoughness.g', 'texelRoughness.r'))
      .replace('#include <metalnessmap_fragment>', THREE.ShaderChunk.metalnessmap_fragment.replace('texelMetalness.b', 'texelMetalness.r'))
      .replace('#include <lights_pars_begin>', THREE.ShaderChunk.lights_pars_begin.replace(
        'return smoothstep( coneCosine, penumbraCosine, angleCosine );',
        'return float( angleCosine > coneCosine );'));
  };
  material.customProgramCacheKey = () => 'native-material-channels-hard-spot-v1';
}

export class SceneLights {
  constructor(scene) {
    this.scene = scene;
    this.items = new Map();
    this.previewMode = 'auto';
    this.preview = new THREE.Group();
    const ambient = new THREE.HemisphereLight(0xddefff, 0x64615d, 2.4);
    ambient.position.set(0, 0, 1);
    const sun = new THREE.DirectionalLight(0xffffff, 2.2);
    sun.position.set(4, -6, 10);
    this.preview.add(ambient, sun);
    scene.add(this.preview);
  }
  remove(id) {
    const light = this.items.get(id);
    if (!light) return;
    this.scene.remove(light, light.target);
    light.shadow?.dispose();
    this.items.delete(id);
  }
  sync(objects, bounds, geometryChanged) {
    for (const id of this.items.keys()) if (!objects.get(id)?.light) this.remove(id);
    for (const [id, object] of objects) {
      const data = object.light;
      if (!data) continue;
      let light = this.items.get(id);
      if (!light || light.userData.type !== data.type) {
        this.remove(id);
        light = data.type === 0 ? new THREE.PointLight() : data.type === 1 ? new THREE.SpotLight() : new THREE.DirectionalLight();
        light.userData.type = data.type;
        light.shadow.mapSize.set(1024, 1024);
        light.shadow.autoUpdate = false;
        light.shadow.bias = -0.0001;
        light.shadow.normalBias = 0.02;
        this.items.set(id, light);
        this.scene.add(light);
        if (light.target) this.scene.add(light.target);
      }
      const signature = JSON.stringify([data, object.position, object.rotation, object.visible]);
      if (geometryChanged || light.userData.signature !== signature) {
        const { color, intensity } = lightParameters(data);
        light.color.copy(color);
        light.intensity = intensity;
        light.visible = object.visible;
        if (light.castShadow && !data.shadows) {
          light.shadow.dispose(); light.shadow.map = null; light.shadow.mapPass = null;
        }
        light.castShadow = data.shadows;
        light.position.fromArray(object.position);
        if (data.type !== 2) { light.distance = 0; light.decay = 2; }
        const direction = new THREE.Vector3(0, 0, -1).applyQuaternion(new THREE.Quaternion().fromArray(object.rotation).normalize());
        if (data.type === 1) {
          light.angle = THREE.MathUtils.clamp(data.angle, 0.0001, Math.PI / 2);
          light.penumbra = 0; // Native edge_blend is currently unused.
        }
        if (light.target) light.target.position.copy(light.position).add(direction);
        this.fitShadow(light, bounds, direction);
        light.shadow.needsUpdate = true;
        light.userData.signature = signature;
      }
    }
    this.updatePreview();
  }
  fitShadow(light, bounds, direction) {
    const box = bounds.isEmpty() ? new THREE.Box3(new THREE.Vector3(-5,-5,-5), new THREE.Vector3(5,5,5)) : bounds;
    const center = box.getCenter(new THREE.Vector3());
    const radius = Math.max(box.getSize(new THREE.Vector3()).length()/2, 1);
    const shadow = light.shadow.camera;
    if (light.isDirectionalLight) {
      // Directional position has no effect on illumination; center its shadow view.
      light.position.copy(center).addScaledVector(direction, -(radius * 2 + 1));
      light.target.position.copy(center);
      shadow.left = shadow.bottom = -radius * 1.05;
      shadow.right = shadow.top = radius * 1.05;
      shadow.near = Math.max(radius * 0.001, 0.01);
      shadow.far = radius * 4 + 2;
      shadow.up.set(0, Math.abs(direction.z) > 0.999 ? 1 : 0, Math.abs(direction.z) > 0.999 ? 0 : 1);
    } else {
      shadow.near = 0.01;
      shadow.far = Math.max(light.position.distanceTo(center) + radius * 1.1, 1);
    }
    shadow.updateProjectionMatrix();
    light.target?.updateMatrixWorld();
  }
  updatePreview() {
    this.preview.visible = this.previewMode === 'on' || (this.previewMode === 'auto' && this.items.size === 0);
  }
  setPreview(mode) { this.previewMode = mode; this.updatePreview(); }
  dispose() { for (const id of [...this.items.keys()]) this.remove(id); this.scene.remove(this.preview); }
}
