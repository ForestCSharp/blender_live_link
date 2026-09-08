import * as THREE from 'three';
import { DebugCamera } from './camera.js';
import { SceneResources } from './resources.js';

const $ = id => document.getElementById(id);
const error = (message = '') => { $('error').textContent = message; $('error').hidden = !message; };
let renderer;
try { renderer = new THREE.WebGLRenderer({ antialias: true }); }
catch (exc) { error(`WebGL2 could not start: ${exc.message}`); throw exc; }
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
renderer.setClearColor(0x14191e);
renderer.shadowMap.enabled = true;
renderer.shadowMap.type = THREE.PCFShadowMap;
renderer.toneMapping = THREE.NoToneMapping;
$('viewport').append(renderer.domElement);
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(60, 1, 0.01, 10000);
const controls = new DebugCamera(camera, renderer.domElement);
let resources = new SceneResources(scene);
let source = 'live', session = '', revision = -1, selection = 0;
let previewMode = 'auto';

function updateCounts() {
  const { meshCount, triangles } = resources.diagnostics();
  $('count').textContent = `${meshCount} visible meshes · ${triangles.toLocaleString()} triangles`;
  $('empty').hidden = meshCount > 0;
}
new ResizeObserver(() => {
  const { clientWidth: width, clientHeight: height } = $('viewport');
  renderer.setSize(width, height);
  camera.aspect = width / Math.max(height, 1);
  camera.updateProjectionMatrix();
}).observe($('viewport'));
let lastFrame = performance.now();
renderer.setAnimationLoop(now => {
  controls.move((now - lastFrame) / 1000); lastFrame = now;
  renderer.render(scene, camera);
});
$('frame').onclick = () => controls.frame(resources.bounds());
$('reset-camera').onclick = () => controls.reset();
$('preview').onchange = () => {
  previewMode = $('preview').value;
  resources.lights.setPreview(previewMode);
};
window.addEventListener('keydown', e => {
  if (e.code === 'KeyF' && !e.ctrlKey && !e.metaKey && !['INPUT','SELECT','TEXTAREA'].includes(e.target.tagName)) controls.frame(resources.bounds());
});
$('live').onclick = () => {
  selection++; source = 'live'; revision = -1;
  $('live').setAttribute('aria-pressed', 'true'); error();
};
$('file').onchange = async () => {
  const file = $('file').files[0];
  if (!file) return;
  const token = ++selection;
  let candidate;
  try {
    if (file.size > 128 * 1024 * 1024 + 4) throw new Error('Snapshot exceeds 128 MiB');
    const response = await fetch('/api/snapshot', { method: 'POST', body: file });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error);
    if (token !== selection) return;
    const stagingScene = new THREE.Scene();
    candidate = new SceneResources(stagingScene);
    await candidate.apply(data);
    if (token !== selection) return;
    resources.dispose();
    for (const object of [...stagingScene.children]) scene.add(object);
    candidate.scene = scene; candidate.lights.scene = scene;
    resources = candidate; candidate = null;
    resources.lights.setPreview(previewMode);
    controls.seed(data.initialCamera, true);
    source = 'file';
    $('live').setAttribute('aria-pressed', 'false');
    $('status').textContent = `Snapshot: ${file.name}`;
    updateCounts();
    error(resources.missingImages ? resources.lastImageError : '');
  } catch (exc) { if (token === selection) error(`Could not open snapshot: ${exc.message}`); }
  finally { candidate?.dispose(); $('file').value = ''; }
};

async function poll() {
  const token = selection;
  try {
    if (source === 'live') {
      const response = await fetch(`/api/scene?since=${revision}&session=${session}`, { cache: 'no-store' });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const data = await response.json();
      if (source === 'live' && token === selection) {
        const target = resources;
        await target.apply(data);
        if (source === 'live' && token === selection && target === resources) {
          if (data.cameraReady) controls.seed(data.initialCamera);
          session = data.session; revision = data.revision;
          resources.lights.setPreview(previewMode);
          updateCounts();
          $('status').textContent = data.connected ? 'Live Blender · Connected' : 'Live Blender · Waiting for connection';
          if (data.error) error(data.error);
        }
      }
    }
    if (resources.missingImages && performance.now() >= resources.imageRetryAt) {
      const target = resources;
      await target.refreshTextures();
      if (!target.disposed) target.sync();
    }
  } catch (exc) {
    if (source === 'live' && token === selection) {
      revision = -1;
      $('status').textContent = `Bridge unavailable: ${exc.message}`;
    }
  }
  setTimeout(poll, 250);
}
poll();
window.gameWebDiagnostics = () => ({ source, revision, session, ...resources.diagnostics(),
  geometries: renderer.info.memory.geometries, gpuTextures: renderer.info.memory.textures,
  cameraUp: camera.up.toArray(), cameraPosition: camera.position.toArray(),
  cameraForward: camera.getWorldDirection(new THREE.Vector3()).toArray(),
  cameraInitialized: controls.initialized, fov: camera.fov, aspect: camera.aspect });
