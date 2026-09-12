import { PART_NAMES } from './gameplay.js';
const element = (tag,text) => {const e=document.createElement(tag);if(text!==undefined)e.textContent=text;return e;};
const button=(text,action)=>{const e=element('button',text);e.onclick=action;return e;};
export class GameplayUI {
  constructor(host) { this.host=host; this.signature=''; }
  update(resources,controls) {
    const game=resources.gameplay;
    const characters=[...resources.objects.values()].filter(o=>o.character).sort((a,b)=>a.id-b.id);
    const templates=[...resources.objects.values()].filter(o=>o.part).sort((a,b)=>a.id-b.id);
    const animations=[...game.playbacks].map(([id,p])=>({key:`Object ${id}`,p}));
    for(const mech of game.mechs.values())for(const [id,p] of mech.armatures)animations.push({key:`Mech ${mech.id} · Armature ${id}`,p});
    const signature=JSON.stringify([game.epoch,game.playerId,controls.debug,game.playing,game.rate,
      characters.map(c=>[c.id,c.name]),templates.map(t=>[t.id,t.name,t.part.type]),
      [...game.mechs.values()].map(m=>[m.id,m.loadout,m.errors]),
      animations.map(a=>[a.key,a.p.clip,a.p.data.animations.map(c=>c.name)]),game.errors]);
    if(this.signature===signature && this.resources===resources)return;
    this.signature=signature;this.resources=resources;this.host.replaceChildren();
    this.host.append(element('p',`Controlled character: ${game.playerId ?? 'none'} · ${controls.debug?'Debug camera':'Character control'}`));
    for(const character of characters) {
      const row=element('section');row.append(element('strong',`${character.name} (${character.id})`));
      const mech=game.mechs.get(character.id);
      row.append(button(mech?'Remove mech':'Create mech',()=>{if(mech)game.remove(character.id);else game.create(character.id);}));
      if(mech) {
        for(let slot=0;slot<5;slot++) {
          const label=element('label',PART_NAMES[slot]);const select=element('select');
          select.setAttribute('aria-label',`${character.id} ${PART_NAMES[slot]}`);
          const add=(value,name)=>{const option=element('option',name);option.value=value;select.append(option);};
          add('','Default');for(const t of templates.filter(t=>t.part.type===slot))add(String(t.id),`${t.name} (${t.id})`);
          const selected=mech.loadout[slot];
          if(selected!==null&&!templates.some(t=>t.id===selected&&t.part.type===slot))add(String(selected),`Missing template (${selected})`);
          select.value=selected===null?'':String(selected);
          select.onchange=()=>game.select(character.id,slot,select.value===''?null:Number(select.value));
          label.append(select);row.append(label);
        }
        for(const error of mech.errors)row.append(element('p',error));
      }
      this.host.append(row);
    }
    const animation=element('section');animation.append(element('strong','Animation'));
    animation.append(button(game.playing?'Pause animation':'Play animation',()=>{game.playing=!game.playing;}),button('Rewind',()=>game.rewind()));
    const label=element('label','Playback rate');const rate=element('input');rate.type='number';rate.min=0;rate.max=4;rate.step=.1;rate.value=game.rate;
    rate.onchange=()=>{game.rate=Math.max(0,Math.min(4,Number(rate.value)||0));};label.append(rate);animation.append(label);
    for(const {key,p} of animations) {
      const label=element('label',key),select=element('select');
      p.data.animations.forEach((clip,index)=>{const option=element('option',clip.name||`Clip ${index}`);option.value=index;select.append(option);});
      select.value=p.clip;select.onchange=()=>{p.select(Number(select.value));game.update();};label.append(select);animation.append(label);
    }
    this.host.append(animation);
    for(const error of game.errors)this.host.append(element('p',error));
  }
}
