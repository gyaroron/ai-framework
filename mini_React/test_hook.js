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
  }

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

console.log("All hook tests passed.");
