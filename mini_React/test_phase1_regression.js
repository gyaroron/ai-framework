const assert = require('node:assert/strict');
const {JSDOM}=require('../_chat_bot_server/node_modules/jsdom');
const path=require('node:path');
(async()=>{
 const env=new JSDOM('');global.document=env.window.document;
 const {createElement:h}=require('./element');const {render}=require('./render');const {diff}=require('./diff');
 const p=document.createElement('div');const a=h('ul',null,h('p',{key:'a'},'A'));const b=h('ul',null,h('span',{key:'a'},'B'));const root=render(a,p);diff(p,root,a,b);assert.equal(root.innerHTML,'<span>B</span>');
 const before=p.innerHTML;assert.throws(()=>diff(p,root,b,h('ul',null,h('p',{key:'a'}),h('p',{key:'a'}))),/Duplicate/);assert.equal(p.innerHTML,before);
 const x=h('ul',null,h('li',null,'plain'),h('li',{key:'__idx_0'},'key'));const y=h('ul',null,h('li',{key:'__idx_0'},'key'),h('li',null,'new'));const q=document.createElement('div');const r=render(x,q);const keyed=r.lastChild;diff(q,r,x,y);assert.equal(r.firstChild,keyed);assert.equal(r.textContent,'keynew');
 const page=await JSDOM.fromFile(path.join(__dirname,'phase1_playground.html'),{resources:'usable',runScripts:'dangerously'});const w=page.window;await new Promise(resolve=>w.addEventListener('load',resolve,{once:true}));const d=w.document;const actual=d.getElementById('actual-surface');const test=d.getElementById('test-surface');const initial=actual.innerHTML;
 test.innerHTML='<p>A</p><p>B</p>';d.getElementById('patch').click();assert.equal(actual.children.length,2);d.getElementById('back').click();assert.equal(actual.innerHTML,initial);d.getElementById('forward').click();assert.equal(actual.textContent,'AB');
 function failOnce(){const original=test.replaceChildren;test.replaceChildren=function(...args){test.replaceChildren=original;throw new Error('injected render failure');};}
 const saved=actual.innerHTML;const position=d.getElementById('history-position').textContent;test.innerHTML='<p>C</p>';failOnce();d.getElementById('patch').click();assert.equal(actual.innerHTML,saved);assert.equal(test.innerHTML,saved);assert.equal(d.getElementById('history-position').textContent,position);
 failOnce();d.getElementById('back').click();assert.equal(actual.innerHTML,saved);assert.equal(d.getElementById('history-position').textContent,position);
 test.innerHTML='';d.getElementById('patch').click();assert.equal(actual.childNodes.length,0);d.getElementById('back').click();assert.equal(actual.innerHTML,saved);
 w.close();env.window.close();console.log('PASS: keyed replacement, duplicate rejection, key isolation, multi-root, empty state, Patch/travel rollback');
})().catch(e=>{console.error(e);process.exitCode=1;});
