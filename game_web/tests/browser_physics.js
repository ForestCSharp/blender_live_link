async (page) => {
  const tab = await page.context().newPage();
  const errors = [];
  const wait = tab.waitForFunction.bind(tab);
  tab.waitForFunction = (fn, ...args) => wait(fn, ...args).catch(error => {
    throw new Error(`${fn}: ${error.message}`);
  });
  tab.on('pageerror', e => errors.push(e.message));
  await tab.route('**/*', route => route.request().url().startsWith('http://127.0.0.1:8000/') ? route.continue() : route.abort());
  try {
    await tab.goto('http://127.0.0.1:8000');
    await tab.waitForFunction(() => window.gameWebDiagnostics?.().physicsState === 'ready');
    const result = await tab.evaluate(async () => {
      const THREE = await import('/vendor/three/three.module.js');
      const {SceneResources} = await import('/resources.js');
      const assert = (ok, text) => { if (!ok) throw new Error(text); };
      const scene = new THREE.Scene(), resources = new SceneResources(scene);
      const positions = [-.5,-.5,-.5,.5,-.5,-.5,-.5,.5,-.5,.5,.5,-.5,-.5,-.5,.5,.5,-.5,.5,-.5,.5,.5,.5,.5,.5];
      const box = (id,z,dynamic=true) => ({id,name:`Box ${id}`,position:[0,0,z],rotation:[0,0,0,1],scale:[1,1,1],visible:true,
        rigidBody:{isDynamic:dynamic,mass:2},mesh:{positions:[...positions],indices:[0,2,1,1,2,3,4,5,6,5,7,6,0,1,4,1,5,4,2,6,3,3,6,7,0,4,2,2,4,6,1,3,5,3,7,5],normals:[],materialIds:[]}});
      const floor = box(1,-.5,false); floor.scale=[20,20,1];
      const body = box(2,4), stacked = box(3,6);
      const sun={id:10,name:'Sun',position:[0,0,8],rotation:[0,0,0,1],scale:[1,1,1],visible:true,light:{type:2,color:[1,1,1],power:1361,shadows:true}};
      let objects=[floor,body,stacked,sun];
      const full = () => ({kind:'full',session:'physics-test',generation:0,objects,materials:[],images:[]});
      try {
        await resources.apply(full());
        assert(resources.physics.diagnostics().dynamicBodies===2,'dynamic count');
        const identity=resources.physics.bodies.get(2).body, geometry=resources.meshes.get(2).geometry;
        const shadow=resources.lights.items.get(10).shadow; shadow.needsUpdate=false;
        resources.stepPhysics(.1);
        assert(shadow.needsUpdate,'Moving bodies did not refresh shadows');
        assert(Math.abs(identity.GetMotionProperties().GetInverseMass()-.5)<1e-6,'Exported mass ignored');
        assert(resources.meshes.get(2).position.z<4,'gravity');
        const before=resources.meshes.get(2).position.z;
        await resources.apply({kind:'delta',batches:[{objects:[],materials:[{id:11,color:[1,0,0,1]}]}]});
        assert(resources.physics.bodies.get(2).body===identity && resources.meshes.get(2).geometry===geometry,'material update replaced resources');
        assert(resources.meshes.get(2).position.z===before,'material update teleported body');
        await resources.apply(JSON.parse(JSON.stringify(full())));
        assert(resources.physics.bodies.get(2).body===identity && resources.meshes.get(2).geometry===geometry,'recovery snapshot replaced resources');
        resources.physics.running=false; resources.stepPhysics(1);
        assert(resources.meshes.get(2).position.z===before,'pause');
        resources.resetPhysics(); assert(resources.meshes.get(2).position.z===4 && !resources.physics.running,'reset');
        resources.physics.running=true;
        for(let i=0;i<300;i++) resources.stepPhysics(1/60);
        const resting=resources.meshes.get(2).position.z, upper=resources.meshes.get(3).position.z;
        assert(Math.abs(resting-.5)<.08 && Math.abs(upper-1.5)<.12,`stacking ${resting}, ${upper}`);
        const stable=resources.meshes.get(2).position.z;
        resources.stepPhysics(5,true); assert(resources.meshes.get(2).position.z===stable,'hidden page');
        const edit={...body,position:[3,0,5]}; delete edit.mesh;
        const other=resources.physics.bodies.get(3).body;
        await resources.apply({kind:'delta',batches:[{objects:[edit],materials:[]}]});
        assert(resources.meshes.get(2).position.z===5 && resources.physics.bodies.get(3).body===other,'edit reset affected neighbor');
        await resources.apply({kind:'delta',batches:[{objects:[{...edit,visible:false}],materials:[]}]});
        assert(resources.physics.diagnostics().dynamicBodies===2,'visibility removed collider');
        await resources.apply({kind:'delta',batches:[{objects:[{...edit,rigidBody:null}],materials:[],deleted:[3]}]});
        assert(resources.physics.diagnostics().dynamicBodies===0,'body deletion/removal');
        const replaced=box(9,8); objects=[floor,replaced];
        await resources.apply({...full(),generation:99});
        resources.stepPhysics(.1);
        replaced.mesh.positions=replaced.mesh.positions.map(v=>v*2);
        await resources.apply({kind:'delta',batches:[{objects:[{id:9,mesh:replaced.mesh}],materials:[]}]});
        assert(resources.meshes.get(9).position.z===8,'Collider replacement did not reset pose');
        await resources.apply({kind:'delta',batches:[{objects:[{id:9,rigidBody:{isDynamic:true,mass:4}}],materials:[]}]});
        assert(Math.abs(resources.physics.bodies.get(9).body.GetMotionProperties().GetInverseMass()-.25)<1e-6,'Mass edit ignored');
        // Off-center mirrored hull retains the exported origin and settles by its geometry.
        const offset=box(4,5); offset.mesh.positions=positions.map((v,i)=>i%3===2?v+2:v);
        offset.scale=[-2,1,1]; offset.rotation=[0,0,Math.sin(.2),Math.cos(.2)];
        objects=[floor,offset]; await resources.apply({...full(),generation:1});
        assert(resources.meshes.get(4).position.z===5,'center-of-mass origin shift');
        for(let i=0;i<300;i++) resources.stepPhysics(1/60);
        assert(Math.abs(resources.meshes.get(4).position.z+1.5)<.1,'scaled offset hull contact');
        // Same duration at 30/60/120 rendering Hz gives the same fixed-step result.
        const zs=[];
        for(const hz of [30,60,120]) {
          objects=[box(5,100)]; await resources.apply({...full(),generation:hz});
          for(let i=0;i<hz;i++) resources.stepPhysics(1/hz);
          zs.push(resources.meshes.get(5).position.z);
        }
        assert(Math.max(...zs)-Math.min(...zs)<.001,`frame rates ${zs}`);
        // Repeated hull/world replacement should plateau, not leak WASM pages.
        const memory=[];
        for(let cycle=0;cycle<100;cycle++) {
          objects=[floor,box(6,3)]; await resources.apply({...full(),generation:1000+cycle});
          for(let j=0;j<10;j++) { resources.resetPhysics(); resources.stepPhysics(1/60); }
          if(cycle===20||cycle===99) memory.push(resources.physics.J.HEAP8.buffer.byteLength);
        }
        assert(memory[1]===memory[0],`WASM memory growth ${memory}`);
        const invalid=box(7,4); invalid.rigidBody.mass=0;
        const badHull=box(8,4); badHull.mesh.positions=[0,0,0]; badHull.mesh.indices=[];
        objects=[invalid,badHull]; await resources.apply({...full(),generation:2000});
        assert(resources.physics.errors.size===2 && resources.meshes.size===2,'invalid bodies must remain visible');
        return {resting,upper,frameRateZ:zs,wasmBytes:memory,invalidErrors:[...resources.physics.errors.values()]};
      } finally { resources.dispose(); }
    });
    await tab.locator('#file').setInputFiles('game_web/tests/physics_snapshot.bin');
    await tab.waitForFunction(() => window.gameWebDiagnostics().source==='file' && window.gameWebDiagnostics().dynamicBodies===1);
    await tab.waitForFunction(() => window.gameWebDiagnostics().objects.some(o=>o.position[2]>.3 && o.position[2]<.7));
    await tab.locator('#physics-toggle').click();
    if ((await tab.evaluate(()=>window.gameWebDiagnostics())).physicsRunning) throw new Error('Pause button');
    await tab.locator('#physics-reset').click();
    const reset=await tab.evaluate(()=>window.gameWebDiagnostics());
    if(!reset.objects.some(o=>o.position[2]===4)) throw new Error('Snapshot reset lost authored pose');
    await tab.evaluate(()=>{const input=document.getElementById('file'),d=new DataTransfer();d.items.add(new File(['bad'],'invalid.bin'));input.files=d.files;input.dispatchEvent(new Event('change'));});
    await tab.waitForFunction(()=>!document.getElementById('error').hidden);
    if((await tab.evaluate(()=>window.gameWebDiagnostics())).dynamicBodies!==1) throw new Error('Invalid snapshot replaced physics');
    await tab.bringToFront();
    await tab.locator('canvas').click();
    await tab.waitForFunction(()=>document.pointerLockElement!==null);
    await tab.keyboard.press('Control+Space');
    await tab.waitForFunction(()=>window.gameWebDiagnostics().physicsRunning);
    await tab.keyboard.press('Control+r');
    await tab.evaluate(()=>document.exitPointerLock());
    await tab.locator('#live').click();
    await tab.waitForFunction(()=>window.gameWebDiagnostics().source==='live' && window.gameWebDiagnostics().physicsRunning);
    if(errors.length) throw new Error(errors.join('\n'));
    return result;
  } finally { await tab.close(); }
}
