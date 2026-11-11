"use strict";

const assert = require("node:assert/strict");
const { createElement } = require("./element");
const { render } = require("./render");
const { diff } = require("./diff");
const { useState, useEffect } = require("./hook");

class MockNode {
  constructor(type, nodeType = 1) {
    this.type = type;
    this.nodeType = nodeType;
    this.children = [];
    this.nodeValue = "";
    this.props = {};
    this.appendCalls = 0;
    this.listeners = new Map();
  }

  addEventListener(type, callback) {
    if (!this.listeners.has(type)) this.listeners.set(type, new Set());
    this.listeners.get(type).add(callback);
  }

  removeEventListener(type, callback) { this.listeners.get(type)?.delete(callback); }

  appendChild(node) {
    this.children.push(node);
    this.appendCalls += 1;
    return node;
  }

  removeChild(node) {
    const index = this.children.indexOf(node);
    assert.notEqual(index, -1);
    this.children.splice(index, 1);
    return node;
  }

  replaceChild(next, previous) {
    const index = this.children.indexOf(previous);
    assert.notEqual(index, -1);
    this.children[index] = next;
    return previous;
  }

  get textContent() {
    return this.nodeType === 3
      ? this.nodeValue
      : this.children.map((child) => child.textContent).join("");
  }
}

global.document = {
  createElement(type) { return new MockNode(type); },
  createTextNode(text) {
    const node = new MockNode("#text", 3);
    node.nodeValue = text;
    return node;
  },
  createDocumentFragment() { return new MockNode("#fragment", 11); },
};

function mount(vnode) {
  const container = new MockNode("#container");
  const dom = render(vnode, container);
  return { container, dom };
}

// 1. Initial state and one direct-value update.
{
  let setCount;
  let renderCount = 0;
  function Counter() {
    const [count, set] = useState(0);
    setCount = set;
    renderCount += 1;
    return createElement("p", null, count);
  }

  const tree = createElement(Counter, null);
  const { dom } = mount(tree);
  assert.equal(dom.textContent, "0");
  setCount(1);
  assert.equal(dom.textContent, "1");
  assert.equal(renderCount, 2);
  console.log("PASS 1: useState initialized and direct update rerendered");
}

// 2. Functional updates accumulate from the latest state.
{
  let incrementTwice;
  function Counter() {
    const [count, setCount] = useState(10);
    incrementTwice = () => {
      setCount((previous) => previous + 1);
      setCount((previous) => previous + 1);
    };
    return createElement("p", null, count);
  }

  const { dom } = mount(createElement(Counter, null));
  assert.equal(dom.textContent, "10");
  incrementTwice();
  assert.equal(dom.textContent, "12");
  console.log("PASS 2: consecutive functional updates accumulated");
}

// 3. Empty dependency list runs once across state-driven rerenders.
{
  let setValue;
  let effectRuns = 0;
  function EffectOnce() {
    const [value, set] = useState(0);
    setValue = set;
    useEffect(() => {
      effectRuns += 1;
    }, []);
    return createElement("p", null, value);
  }

  const { dom } = mount(createElement(EffectOnce, null));
  assert.equal(effectRuns, 1);
  setValue(1);
  assert.equal(dom.textContent, "1");
  assert.equal(effectRuns, 1);
  console.log("PASS 3: useEffect with [] ran once on mount");
}

// 4. Dependency change runs previous cleanup before the next effect.
{
  const events = [];
  function Watcher(props) {
    useEffect(() => {
      events.push(`effect:${props.value}`);
      return () => events.push(`cleanup:${props.value}`);
    }, [props.value]);
    return createElement("p", null, props.value);
  }

  const oldTree = createElement(Watcher, { value: "old" });
  const newTree = createElement(Watcher, { value: "new" });
  const { container, dom } = mount(oldTree);
  assert.deepEqual(events, ["effect:old"]);

  const updatedDom = diff(container, dom, oldTree, newTree);
  assert.equal(updatedDom, dom);
  assert.equal(dom.textContent, "new");
  assert.deepEqual(events, ["effect:old", "cleanup:old", "effect:new"]);
  console.log("PASS 4: dependency change cleaned up before running next effect");
}

// 5. Finish the current effect batch before processing synchronous setters.
{
  const events = [];
  function Effects() {
    const [value, setValue] = useState(0);
    useEffect(() => { events.push("first"); setValue(1); }, []);
    useEffect(() => {
      events.push(`effect:${value}`);
      return () => events.push(`cleanup:${value}`);
    }, [value]);
    useEffect(() => { events.push("once"); }, []);
    return createElement("p", null, value);
  }
  const { dom } = mount(createElement(Effects, null));
  assert.equal(dom.textContent, "1");
  assert.deepEqual(events, ["first", "effect:0", "once", "cleanup:0", "effect:1"]);
  console.log("PASS 5: synchronous effect setters preserve batch order");
}

// 6. All removal paths recursively clean up and disable stale setters.
for (const removal of ["delete", "replace", "keyed"]) {
  let setter;
  let cleanups = 0;
  let renders = 0;
  function Label(props) { return createElement("em", null, props.text); }
  function Stateful() {
    const [changed, setChanged] = useState(false);
    setter = setChanged;
    renders += 1;
    useEffect(() => () => { cleanups += 1; setChanged(true); }, []);
    return createElement(changed ? "span" : "p", null,
      createElement(Label, { text: "child" }));
  }
  const component = createElement(Stateful, { key: "removed" });
  const oldTree = removal === "keyed" ? createElement("div", null, component) : component;
  const { container, dom } = mount(oldTree);
  const { getComponentInstance } = require("./component");
  const instance = getComponentInstance(component);
  const next = removal === "delete" ? null : createElement(removal === "keyed" ? "div" : "article", null);
  diff(container, dom, oldTree, next);
  assert.equal(cleanups, 1, removal);
  assert.equal(instance.dom, null, removal);
  assert.equal(instance.renderedVNode, null, removal);
  assert.doesNotThrow(() => { setter(false); setter(true); });
  assert.equal(renders, 1, removal);
  require("./component").unmount(component);
  assert.equal(cleanups, 1, "cleanup must be idempotent");
}
console.log("PASS 6: delete, replacement and keyed removal unmount the root and its stateless child");

// 7. Hooks are limited to the root component; a child that calls one is rejected.
for (const hookName of ["useState", "useEffect"]) {
  function StatefulChild() {
    if (hookName === "useState") useState(0);
    else useEffect(() => {}, []);
    return createElement("p", null, "child");
  }
  function Root() {
    return createElement("section", null, createElement(StatefulChild, null));
  }
  assert.throws(() => mount(createElement(Root, null)),
    new RegExp(`${hookName} can only be used in the root component`));
}
console.log("PASS 7: hooks called inside a child component throw");

// 8. Root state flows to stateless children through props (Lifting State Up).
{
  let childRenders = 0;
  function CountLabel(props) {
    childRenders += 1;
    return createElement("span", { id: "count" }, `count: ${props.count}`);
  }
  function AddButton(props) {
    return createElement("button", { onClick: props.onAdd }, "add");
  }
  function Root() {
    const [count, setCount] = useState(0);
    return createElement("section", null,
      createElement(CountLabel, { count }),
      createElement(AddButton, { onAdd: () => setCount((value) => value + 1) }));
  }
  const { dom } = mount(createElement(Root, null));
  const labelNode = dom.children[0];
  const buttonNode = dom.children[1];
  assert.equal(labelNode.textContent, "count: 0");
  for (const handleClick of Array.from(buttonNode.listeners.get("click"))) handleClick({});
  assert.equal(dom.children[0], labelNode, "child DOM node should be reused");
  assert.equal(labelNode.textContent, "count: 1");
  assert.equal(childRenders, 2);
  console.log("PASS 8: root state reached stateless children through props");
}
console.log("All hook tests passed.");
