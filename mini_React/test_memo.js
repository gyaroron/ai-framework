"use strict";

const assert = require("node:assert/strict");
const { createElement } = require("./element");
const { render } = require("./render");
const { diff } = require("./diff");
const { useMemo, useState } = require("./hook");

class MockNode {
  constructor(type, nodeType = 1) {
    this.type = type;
    this.nodeType = nodeType;
    this.children = [];
    this.nodeValue = "";
  }
  appendChild(node) { this.children.push(node); return node; }
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

// Stable dependencies cache the computed value across state-driven renders.
{
  let computeCount = 0;
  let increment;
  function MemoCounter() {
    const [count, setCount] = useState(0);
    increment = () => setCount((previous) => previous + 1);
    const memoized = useMemo(() => {
      computeCount += 1;
      return `cached:${count}`;
    }, []);
    return createElement("p", null, `${memoized}/${count}`);
  }
  const container = new MockNode("#container");
  const vnode = createElement(MemoCounter, null);
  const dom = render(vnode, container);
  assert.equal(dom.textContent, "cached:0/0");
  increment();
  assert.equal(dom.textContent, "cached:0/1");
  assert.equal(computeCount, 1);
  console.log("PASS 1: useMemo reused its value when dependencies stayed equal");
}

// Changed dependency values cause exactly one recomputation on the next render.
{
  let computeCount = 0;
  function Label(props) {
    const label = useMemo(() => {
      computeCount += 1;
      return `value:${props.value}`;
    }, [props.value]);
    return createElement("p", null, label);
  }
  const oldVNode = createElement(Label, { value: 1 });
  const newVNode = createElement(Label, { value: 2 });
  const container = new MockNode("#container");
  const dom = render(oldVNode, container);
  assert.equal(dom.textContent, "value:1");
  diff(container, dom, oldVNode, newVNode);
  assert.equal(dom.textContent, "value:2");
  assert.equal(computeCount, 2);
  console.log("PASS 2: useMemo recomputed after a dependency changed");
}

// Dependency checks use Object.is, including its NaN and signed-zero behavior.
{
  let renders = 0;
  function Value(props) {
    const result = useMemo(() => ++renders, [props.dep]);
    return createElement("p", null, result);
  }
  const first = createElement(Value, { dep: Number.NaN });
  const sameByObjectIs = createElement(Value, { dep: Number.NaN });
  const container = new MockNode("#container");
  const dom = render(first, container);
  diff(container, dom, first, sameByObjectIs);
  assert.equal(renders, 1);

  const negativeZero = createElement(Value, { dep: -0 });
  diff(container, dom, sameByObjectIs, negativeZero);
  assert.equal(renders, 2);
  console.log("PASS 3: useMemo dependency comparison follows Object.is");
}

console.log("All useMemo tests passed.");
