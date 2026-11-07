"use strict";

const assert = require("node:assert/strict");
const { createElement } = require("./element");
const { render } = require("./render");
const { diff } = require("./diff");
const { FunctionComponent } = require("./component");

class MockNode {
  constructor(type, nodeType = 1) {
    this.type = type;
    this.nodeType = nodeType;
    this.children = [];
    this.props = {};
    this.nodeValue = "";
    this.appendCalls = 0;
    this.replaceCalls = 0;
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
    const existing = this.children.indexOf(next);
    if (existing !== -1) this.children.splice(existing, 1);
    this.children[this.children.indexOf(previous)] = next;
    this.replaceCalls += 1;
    return previous;
  }

  set id(value) { this.props.id = value; }
  get id() { return this.props.id || ""; }
  set className(value) { this.props.className = value; }
  get className() { return this.props.className || ""; }
  set title(value) { this.props.title = value; }
  get title() { return this.props.title || ""; }
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

function mount(element) {
  const container = new MockNode("#container");
  const dom = render(element, container);
  return { container, dom };
}

// 1. Basic function component mount.
{
  function Header(props) {
    return createElement("h1", null, props.title);
  }
  const tree = createElement(Header, { title: "Mini React" });
  const { dom } = mount(tree);

  assert.equal(dom.type, "h1");
  assert.equal(dom.textContent, "Mini React");
  console.log("PASS 1: function component mounted its returned host element");
}

// 2. Component props are passed to the returned host tree.
{
  function Counter(props) {
    return createElement("p", { id: `counter-${props.name}` },
      `${props.name}: ${props.count}`);
  }
  const tree = createElement(Counter, { name: "visits", count: 3 });
  const { dom } = mount(tree);

  assert.equal(dom.id, "counter-visits");
  assert.equal(dom.textContent, "visits: 3");
  console.log("PASS 2: component props reached its host element and text");
}

// 3. A parent component can return a tree containing a child component.
{
  function Item(props) {
    return createElement("li", null, props.label);
  }
  function List(props) {
    return createElement("ul", null,
      ...props.items.map((label) => createElement(Item, { label })));
  }
  const tree = createElement(List, { items: ["Alpha", "Beta"] });
  const { dom } = mount(tree);

  assert.equal(dom.type, "ul");
  assert.equal(dom.children.length, 2);
  assert.equal(dom.children[0].textContent, "Alpha");
  assert.equal(dom.children[1].textContent, "Beta");
  console.log("PASS 3: nested function components assembled the host tree");
}

// 4. Same component type is called again and its old host tree is diffed.
{
  let calls = 0;
  function Greeting(props) {
    calls += 1;
    return createElement("div", { className: props.className },
      createElement("span", null, props.name));
  }
  const oldTree = createElement(Greeting, { className: "old", name: "Ari" });
  const newTree = createElement(Greeting, { className: "new", name: "Bo" });
  const { container, dom } = mount(oldTree);
  const oldSpan = dom.children[0];
  const beforeParentAppend = container.appendCalls;
  const beforeChildAppend = dom.appendCalls;
  const result = diff(container, dom, oldTree, newTree);

  assert.equal(calls, 2, "component should run once at mount and once at update");
  assert.equal(result, dom, "same host type should retain the root DOM node");
  assert.equal(container.children[0], dom);
  assert.equal(dom.className, "new");
  assert.equal(dom.children[0], oldSpan, "unchanged host node should be retained");
  assert.equal(oldSpan.textContent, "Bo");
  assert.equal(container.appendCalls, beforeParentAppend);
  assert.equal(dom.appendCalls, beforeChildAppend);
  console.log("PASS 4: component rerender diffed props with minimal DOM mutation");
}

// 5. The public class wrapper exposes hooks and delegates mount/update to the core.
{
  let instance;
  function Card(props) {
    return createElement("article", { className: props.kind }, props.title);
  }
  instance = new FunctionComponent(Card, { kind: "old", title: "Before" });
  assert.ok(Array.isArray(instance.hooks));
  const container = new MockNode("#container");
  const dom = instance.mount(container, render);
  assert.equal(dom.className, "old");
  assert.equal(dom.textContent, "Before");
  const updated = instance.update({ kind: "new", title: "After" },
    container, dom, diff);
  assert.equal(updated, dom);
  assert.equal(updated.className, "new");
  assert.equal(updated.textContent, "After");
  console.log("PASS 5: FunctionComponent class mounted and updated through diff");
}

console.log("All component tests passed.");
