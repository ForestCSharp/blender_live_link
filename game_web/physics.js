import initJolt from './vendor/jolt/jolt-physics.wasm.js';

let runtime;
const load = () => runtime ||= initJolt();
const STEP = 1 / 60;

// Jolt owns bodies and retained shapes; only explicitly constructed temporaries
// are destroyed here. Getters return borrowed wrappers, never owned allocations.
export class ScenePhysics {
  constructor() {
    this.bodies = new Map(); this.errors = new Map(); this.running = true;
    this.accumulator = 0; this.state = 'loading'; this.disposed = false;
    this.ready = load().then(J => {
      if (this.disposed) return;
      this.J = J;
      const settings = new J.JoltSettings();
      settings.mMaxBodies = 1024;
      const pairs = new J.ObjectLayerPairFilterTable(2);
      pairs.EnableCollision(0, 1); pairs.EnableCollision(1, 1);
      const broad = new J.BroadPhaseLayerInterfaceTable(2, 2);
      const a = new J.BroadPhaseLayer(0), b = new J.BroadPhaseLayer(1);
      broad.MapObjectToBroadPhaseLayer(0, a); broad.MapObjectToBroadPhaseLayer(1, b);
      J.destroy(a); J.destroy(b);
      settings.mObjectLayerPairFilter = pairs;
      settings.mBroadPhaseLayerInterface = broad;
      settings.mObjectVsBroadPhaseLayerFilter = new J.ObjectVsBroadPhaseLayerFilterTable(broad, 2, pairs, 2);
      this.world = new J.JoltInterface(settings);
      J.destroy(settings);
      this.interface = this.world.GetPhysicsSystem().GetBodyInterface();
      const gravity = new J.Vec3(0, 0, -10);
      this.world.GetPhysicsSystem().SetGravity(gravity); J.destroy(gravity);
      this.state = 'ready';
    }).catch(error => { this.state = `Physics unavailable: ${error.message}`; });
  }
  remove(id) {
    const record = this.bodies.get(id);
    if (record?.body) {
      this.interface.RemoveBody(record.body.GetID());
      this.interface.DestroyBody(record.body.GetID());
    }
    this.bodies.delete(id); this.errors.delete(id);
  }
  reconcile(objects) {
    if (!this.world) return;
    for (const id of this.bodies.keys()) if (!objects.has(id)) this.remove(id);
    for (const [id, object] of objects) {
      if (object.part || object.attachment || (!object.character && (!object.rigidBody || !object.mesh))) { this.remove(id); continue; }
      const old = this.bodies.get(id);
      const pose = JSON.stringify(object.character
        ? [object.position, object.rotation, object.character.height, object.character.radius]
        : [object.position, object.rotation, object.scale, object.rigidBody]);
      const points = object.character ? [] : object.mesh.positions;
      if (old && old.pose === pose && (old.points === points ||
          old.points.length === points.length && old.points.every((v, i) => v === points[i]))) continue;
      this.remove(id);
      const record = { pose, points, dynamic: object.character ? true : object.rigidBody.isDynamic, character: !!object.character };
      this.bodies.set(id, record);
      try { record.body = object.character ? this.createCharacter(object) : this.create(object); }
      catch (error) { this.errors.set(id, `${object.name || id}: ${error.message}`); }
    }
  }
  createCharacter(object) {
    const J = this.J, owned = [];
    const own = v => { owned.push(v); return v; };
    try {
      const { height, radius } = object.character;
      if (!(height >= 0 && radius > 0)) throw new Error('Invalid capsule dimensions');
      const capsule = new J.CapsuleShapeSettings(height / 2, radius);
      // Decorated settings retain the inner settings. Release the outer owner
      // rather than manually destroying the retained inner allocation.
      const zero = own(new J.Vec3(0, 0, 0));
      const axisRotation = own(new J.Quat(Math.SQRT1_2, 0, 0, Math.SQRT1_2));
      const shape = own(new J.RotatedTranslatedShapeSettings(zero, axisRotation, capsule));
      const result = shape.Create();
      if (result.HasError()) throw new Error(result.GetError().c_str());
      const position = own(new J.RVec3(...object.position));
      const length = Math.hypot(...object.rotation);
      if (!length) throw new Error('Invalid character rotation');
      const rotation = own(new J.Quat(...object.rotation.map(v => v / length)));
      const settings = own(new J.BodyCreationSettings(result.Get(), position, rotation, J.EMotionType_Dynamic, 1));
      settings.mAllowedDOFs = J.EAllowedDOFs_TranslationX | J.EAllowedDOFs_TranslationY | J.EAllowedDOFs_TranslationZ;
      settings.mOverrideMassProperties = J.EOverrideMassProperties_MassAndInertiaProvided;
      settings.mMassPropertiesOverride.mMass = 80;
      settings.mFriction = 0.5;
      settings.mGravityFactor = 1;
      const body = this.interface.CreateBody(settings);
      if (!J.getPointer(body)) throw new Error('Physics body capacity exceeded');
      this.interface.AddBody(body.GetID(), J.EActivation_Activate);
      return body;
    } finally { for (const value of owned.reverse()) J.destroy(value); }
  }
  position(id, fallback) {
    const body = this.bodies.get(id)?.body;
    if (!body) return fallback;
    const p = body.GetPosition();
    return [p.GetX(), p.GetY(), p.GetZ()];
  }
  create(object) {
    const J = this.J, owned = [];
    const own = value => { owned.push(value); return value; };
    try {
      const { isDynamic, mass } = object.rigidBody;
      if (!Number.isFinite(mass) || isDynamic && mass <= 0) throw new Error('Invalid rigid-body mass');
      if (object.scale.some(v => !Number.isFinite(v) || v === 0)) throw new Error('Invalid collider scale');
      const hull = own(new J.ConvexHullShapeSettings());
      const points = object.mesh.positions;
      // Signed scale baked into points supports mirrored and nonuniform hulls.
      const p = own(new J.Vec3(0, 0, 0));
      for (let i = 0; i < points.length; i += 3) {
        const scaled = [0, 1, 2].map(axis => Math.fround(points[i + axis] * object.scale[axis]));
        if (!scaled.every(Number.isFinite)) throw new Error('Collider coordinates exceed physics precision');
        p.Set(...scaled);
        hull.mPoints.push_back(p);
      }
      const result = hull.Create();
      if (result.HasError()) throw new Error(result.GetError().c_str());
      const pos = own(new J.RVec3(...object.position));
      const length = Math.hypot(...object.rotation);
      if (!length) throw new Error('Invalid collider rotation');
      const rot = own(new J.Quat(...object.rotation.map(v => v / length)));
      const settings = own(new J.BodyCreationSettings(result.Get(), pos, rot,
        isDynamic ? J.EMotionType_Dynamic : J.EMotionType_Static, isDynamic ? 1 : 0));
      if (isDynamic) {
        settings.mOverrideMassProperties = J.EOverrideMassProperties_CalculateInertia;
        settings.mMassPropertiesOverride.mMass = mass;
      }
      const body = this.interface.CreateBody(settings);
      if (!J.getPointer(body)) throw new Error('Physics body capacity exceeded');
      this.interface.AddBody(body.GetID(), isDynamic ? J.EActivation_Activate : J.EActivation_DontActivate);
      return body;
    } finally { for (const item of owned.reverse()) J.destroy(item); }
  }
  write(meshes) {
    let moved = false;
    for (const [id, record] of this.bodies) {
      const mesh = meshes.get(id), body = record.body;
      if (!body || !mesh) continue;
      const p = body.GetPosition(), q = body.GetRotation();
      const xyz = [p.GetX(), p.GetY(), p.GetZ()], xyzw = [q.GetX(), q.GetY(), q.GetZ(), q.GetW()];
      if (!mesh.position.equals({x:xyz[0],y:xyz[1],z:xyz[2]}) ||
          mesh.quaternion.toArray().some((v,i) => v !== xyzw[i])) moved = true;
      mesh.position.fromArray(xyz); mesh.quaternion.fromArray(xyzw);
    }
    return moved;
  }
  step(dt, meshes, hidden = false, beforeStep = () => {}, afterStep = () => {}) {
    this.lastSteps = 0;
    if (!this.world || !this.running || hidden) { this.accumulator = 0; return false; }
    this.accumulator += Math.min(Math.max(dt, 0), 5 * STEP);
    let steps = 0;
    while (this.accumulator >= STEP && steps++ < 5) { beforeStep(STEP); this.world.Step(STEP, 1); afterStep(STEP); this.accumulator -= STEP; }
    this.lastSteps = steps;
    return this.write(meshes);
  }
  reset(objects) {
    for (const [id, record] of [...this.bodies]) if (!record.character) this.remove(id);
    this.accumulator = 0; this.reconcile(objects);
  }
  diagnostics() {
    const records = [...this.bodies.values()].filter(r => r.body);
    return { physicsState: this.state, physicsRunning: this.running,
      characterBodies: records.filter(r => r.character).length,
      dynamicBodies: records.filter(r => r.dynamic && !r.character).length, staticBodies: records.filter(r => !r.dynamic).length,
      physicsErrors: [...this.errors.values()] };
  }
  dispose() {
    this.disposed = true;
    for (const id of [...this.bodies.keys()]) this.remove(id);
    if (this.world) { this.J.destroy(this.world); this.world = null; }
  }
}
