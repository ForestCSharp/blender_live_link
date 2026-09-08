import * as THREE from 'three';
import { OrbitControls } from './vendor/three/OrbitControls.js';

const $ = (id) => document.getElementById(id);
const error = (message = '') => { $('error').textContent = message; $('error').hidden = !message; };
let renderer;
try { renderer = new THREE.WebGLRenderer({ antialias: true }); }
catch (exc) { error(`WebGL2 could not start: ${exc.message}`); throw exc; }
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
renderer.setClearColor(0x14191e);
$('viewport').append(renderer.domElement);
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(50, 1, 0.01, 10000);
camera.up.set(0, 0, 1);
camera.position.set(6, -8, 5);
const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;
const ambient = new THREE.HemisphereLight(0xddefff, 0x64615d, 2.4);
ambient.position.set(0, 0, 1);
scene.add(ambient);
const sun = new THREE.DirectionalLight(0xffffff, 2.2);
sun.position.set(4, -6, 10);
scene.add(sun);
let group = new THREE.Group();
scene.add(group);
let session = '';
let source = 'live', revision = -1, selection = 0, frameNext = true;
let meshCount = 0, triangles = 0;

function dispose(root) {
  root.traverse((object) => {
    object.geometry?.dispose();
    if (object.material) object.material.dispose();
  });
}

function replaceScene(data) {
  const next = new THREE.Group();
  const materials = new Map(data.materials.map((m) => [m.id, m.color]));
  let count = 0, triangleCount = 0;
  try {
    for (const object of data.objects) {
      const mesh = object.mesh;
      if (!mesh?.positions.length || !mesh.indices.length) continue;
      const geometry = new THREE.BufferGeometry();
      const color = materials.get(mesh.materialIds[0]) || [0.57, 0.64, 0.69, 1];
      const material = new THREE.MeshStandardMaterial({
        color: new THREE.Color().setRGB(...color.slice(0, 3), THREE.LinearSRGBColorSpace),
        roughness: 0.8, metalness: 0, side: THREE.DoubleSide,
        opacity: Math.max(0, Math.min(1, color[3])), transparent: color[3] < 1,
      });
      const rendered = new THREE.Mesh(geometry, material);
      next.add(rendered);
      geometry.setAttribute('position', new THREE.Float32BufferAttribute(mesh.positions, 3));
      geometry.setIndex(mesh.indices);
      if (mesh.normals.length) geometry.setAttribute('normal', new THREE.Float32BufferAttribute(mesh.normals, 3));
      else geometry.computeVertexNormals();
      rendered.name = object.name;
      rendered.userData.uid = object.id;
      rendered.position.fromArray(object.position);
      rendered.quaternion.fromArray(object.rotation).normalize();
      rendered.scale.fromArray(object.scale);
      rendered.visible = object.visible;
      if (object.visible) { count++; triangleCount += mesh.indices.length / 3; }
    }
  } catch (exc) { dispose(next); throw exc; }
  scene.remove(group);
  dispose(group);
  group = next;
  scene.add(group);
  meshCount = count;
  triangles = triangleCount;
  $('count').textContent = `${count} visible meshes · ${triangleCount.toLocaleString()} triangles`;
  $('empty').hidden = count > 0;
  if (frameNext && count) { frameScene(); frameNext = false; }
}

function frameScene() {
  const box = new THREE.Box3();
  group.updateMatrixWorld(true);
  for (const child of group.children) if (child.visible) box.expandByObject(child);
  if (box.isEmpty()) return;
  const center = box.getCenter(new THREE.Vector3());
  const radius = Math.max(box.getSize(new THREE.Vector3()).length() / 2, 0.01);
  const vertical = THREE.MathUtils.degToRad(camera.fov / 2);
  const horizontal = Math.atan(Math.tan(vertical) * camera.aspect);
  const distance = radius / Math.sin(Math.min(vertical, horizontal)) * 1.15;
  const direction = camera.position.clone().sub(controls.target).normalize();
  if (!direction.lengthSq()) direction.set(1, -1, 1).normalize();
  camera.position.copy(center).addScaledVector(direction, distance);
  camera.near = Math.max(radius / 1000, 0.00001);
  camera.far = Math.max(distance * 100, 100);
  camera.updateProjectionMatrix();
  controls.target.copy(center);
  controls.update();
}

new ResizeObserver(() => {
  const { clientWidth: width, clientHeight: height } = $('viewport');
  renderer.setSize(width, height);
  camera.aspect = width / Math.max(height, 1);
  camera.updateProjectionMatrix();
}).observe($('viewport'));
renderer.setAnimationLoop(() => { controls.update(); renderer.render(scene, camera); });
$('frame').onclick = frameScene;
window.addEventListener('keydown', (event) => { if (event.key.toLowerCase() === 'f') frameScene(); });
$('live').onclick = () => {
  selection++;
  source = 'live'; revision = -1; frameNext = true;
  $('live').setAttribute('aria-pressed', 'true');
  error();
};
$('file').onchange = async () => {
  const file = $('file').files[0];
  if (!file) return;
  const token = ++selection;
  try {
    if (file.size > 128 * 1024 * 1024 + 4) throw new Error('Snapshot exceeds 128 MiB');
    const response = await fetch('/api/snapshot', { method: 'POST', body: file });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error);
    if (token !== selection) return;
    frameNext = true;
    replaceScene(data);
    source = 'file';
    $('live').setAttribute('aria-pressed', 'false');
    $('status').textContent = `Snapshot: ${file.name}`;
    error();
  } catch (exc) { if (token === selection) error(`Could not open snapshot: ${exc.message}`); }
  finally { $('file').value = ''; }
};

async function poll() {
  const token = selection;
  if (source === 'live') {
    try {
      const response = await fetch(`/api/scene?since=${revision}&session=${session}`, { cache: 'no-store' });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const data = await response.json();
      if (source === 'live' && token === selection) {
        if (data.objects) {
          if (data.session !== session) frameNext = true;
          session = data.session;
          replaceScene(data);
          revision = data.revision;
        }
        $('status').textContent = data.connected ? 'Live Blender · Connected' : 'Live Blender · Waiting for connection';
        if (data.error) error(data.error);
      }
    } catch (exc) {
      if (source === 'live' && token === selection) $('status').textContent = `Bridge unavailable: ${exc.message}`;
    }
  }
  setTimeout(poll, 250);
}
poll();
// Read-only diagnostics for browser smoke tests and resource-lifetime checks.
window.gameWebDiagnostics = () => ({ source, revision, meshCount, triangles,
  geometries: renderer.info.memory.geometries, cameraUp: camera.up.toArray(),
  aspect: camera.aspect, objects: group.children.map((o) => ({ id: o.userData.uid,
    position: o.position.toArray(), scale: o.scale.toArray(), visible: o.visible })) });
