"use strict";

const assert = require("node:assert/strict");
const { createElement } = require("./element");
const { render } = require("./render");
const { diff } = require("./diff");

class MockNode {
  constructor(type, nodeType = 1) {
    this.type = type;
    this.nodeType = nodeType;
    this.children = [];
    this.props = {};
    this.nodeValue = "";
    this.appendCalls = 0;
    this.removeCalls = 0;
    this.replaceCalls = 0;
    this.addedListeners = [];
    this.removedListeners = [];
  }

  appendChild(node) {
    this.children.push(node);
    this.appendCalls += 1;
    return node;
  }

  removeChild(node) {
    const index = this.children.indexOf(node);
    assert.notEqual(index, -1, "removeChild target must be a child");
    this.children.splice(index, 1);
    this.removeCalls += 1;
    return node;
  }

  replaceChild(newNode, oldNode) {
    const index = this.children.indexOf(oldNode);
    assert.notEqual(index, -1, "replaceChild target must be a child");
    const existingIndex = this.children.indexOf(newNode);
    if (existingIndex !== -1) {
      this.children.splice(existingIndex, 1);
    }
    const targetIndex = this.children.indexOf(oldNode);
    this.children[targetIndex] = newNode;
    this.replaceCalls += 1;
    return oldNode;
  }

  addEventListener(type, listener) {
    this.addedListeners.push({ type, listener });
  }

  removeEventListener(type, listener) {
    this.removedListeners.push({ type, listener });
  }

  removeAttribute(name) {
    delete this.props[name === "class" ? "className" : name];
  }

  set id(value) { this.props.id = value; }
  get id() { return this.props.id || ""; }
  set className(value) { this.props.className = value; }
  get className() { return this.props.className || ""; }
  set textContent(value) { this._textContent = value; }
  get textContent() {
    if (this.nodeType === 3) return this.nodeValue;
    return this._textContent !== undefined
      ? this._textContent
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
  createDocumentFragment() {
    return new MockNode("#fragment", 11);
  },
};

function mount(vnode) {
  const container = new MockNode("#container");
  const dom = render(vnode, container);
  return { container, dom };
}

// 1. Same-tag properties and text node update while retaining the element.
{
  const oldTree = createElement("div", {
    className: "old", id: "before", textContent: "old text",
  });
  const newTree = createElement("div", {
    className: "new", id: "after", textContent: "new text",
  });
  const { container, dom } = mount(oldTree);
  const result = diff(container, dom, oldTree, newTree);

  assert.equal(result, dom, "same-type update should preserve the DOM node");
  assert.equal(container.children[0], dom);
  assert.equal(dom.className, "new");
  assert.equal(dom.id, "after");
  assert.equal(dom.textContent, "new text");

  const oldText = createElement("#unused", null, "old").props.children[0];
  const newText = createElement("#unused", null, "new").props.children[0];
  const textNode = document.createTextNode("old");
  diff({ appendChild() {} }, textNode, oldText, newText);
  assert.equal(textNode.nodeValue, "new");
  console.log("PASS 1: same node retained; properties, textContent, and text node updated");
}

// 2. A changed element type replaces its current DOM node.
{
  const oldTree = createElement("p", null, "before");
  const newTree = createElement("span", null, "after");
  const { container, dom } = mount(oldTree);
  const result = diff(container, dom, oldTree, newTree);

  assert.equal(container.replaceCalls, 1);
  assert.equal(container.children.length, 1);
  assert.equal(container.children[0], result);
  assert.equal(result.type, "span");
  console.log("PASS 2: changed element type replaced the old node");
}

// 3. A new child is appended without replacing existing children.
{
  const oldTree = createElement("ul", null, createElement("li", null, "A"));
  const newTree = createElement("ul", null,
    createElement("li", null, "A"),
    createElement("li", null, "B"),
  );
  const { dom } = mount(oldTree);
  const previousFirstChild = dom.children[0];
  const beforeAppendCalls = dom.appendCalls;

  diff(dom, dom, oldTree, newTree);

  assert.equal(dom.children.length, 2);
  assert.equal(dom.children[0], previousFirstChild);
  assert.equal(dom.children[1].type, "li");
  assert.equal(dom.children[1].textContent, "B");
  assert.equal(dom.appendCalls - beforeAppendCalls, 1);
  console.log("PASS 3: new child appended while existing child was retained");
}

// 4. A removed virtual child is detached from its parent.
{
  const oldTree = createElement("ul", null,
    createElement("li", null, "A"),
    createElement("li", null, "B"),
  );
  const newTree = createElement("ul", null, createElement("li", null, "A"));
  const { dom } = mount(oldTree);
  const removedChild = dom.children[1];

  diff(dom, dom, oldTree, newTree);

  assert.equal(dom.removeCalls, 1);
  assert.equal(dom.children.length, 1);
  assert.equal(dom.children[0].textContent, "A");
  assert.equal(dom.children.includes(removedChild), false);
  console.log("PASS 4: removed child detached from its parent");
}

// Event properties are detached and reattached when their function changes.
{
  const firstHandler = () => {};
  const secondHandler = () => {};
  const node = new MockNode("button");
  const { updateDom } = require("./diff");
  updateDom(node, { onClick: firstHandler }, { onClick: secondHandler });
  // DOM에는 batch로 감싼 handler가 붙으므로, 뗄 때와 붙일 때 모두 감싼 함수로 비교한다.
  const { toBatchedListener } = require("./hook");
  assert.deepEqual(node.removedListeners,
    [{ type: "click", listener: toBatchedListener(firstHandler) }]);
  assert.deepEqual(node.addedListeners,
    [{ type: "click", listener: toBatchedListener(secondHandler) }]);
  assert.equal(toBatchedListener(firstHandler), toBatchedListener(firstHandler),
    "the same handler must always map to the same wrapped listener");
  console.log("PASS: changed event handler was detached and reattached");
}

// 5. Multiple child removal (Limitation 3 fix verification)
{
  // 5a. A B C D -> A B (removes 2 children without exception)
  {
    const oldTree = createElement("ul", null,
      createElement("li", null, "A"),
      createElement("li", null, "B"),
      createElement("li", null, "C"),
      createElement("li", null, "D"),
    );
    const newTree = createElement("ul", null,
      createElement("li", null, "A"),
      createElement("li", null, "B"),
    );
    const { dom } = mount(oldTree);
    diff(dom, dom, oldTree, newTree);
    assert.equal(dom.children.length, 2);
    assert.equal(dom.children[0].textContent, "A");
    assert.equal(dom.children[1].textContent, "B");
    assert.equal(dom.removeCalls, 2);
  }

  // 5b. A B C D -> empty ul (removes 4 children without exception)
  {
    const oldTree = createElement("ul", null,
      createElement("li", null, "A"),
      createElement("li", null, "B"),
      createElement("li", null, "C"),
      createElement("li", null, "D"),
    );
    const newTree = createElement("ul", null);
    const { dom } = mount(oldTree);
    diff(dom, dom, oldTree, newTree);
    assert.equal(dom.children.length, 0);
    assert.equal(dom.removeCalls, 4);
  }

  // 5c. A B C -> A (removes 2 children without exception)
  {
    const oldTree = createElement("ul", null,
      createElement("li", null, "A"),
      createElement("li", null, "B"),
      createElement("li", null, "C"),
    );
    const newTree = createElement("ul", null, createElement("li", null, "A"));
    const { dom } = mount(oldTree);
    diff(dom, dom, oldTree, newTree);
    assert.equal(dom.children.length, 1);
    assert.equal(dom.children[0].textContent, "A");
    assert.equal(dom.removeCalls, 2);
  }
  console.log("PASS 5: multiple children removed cleanly without index shift exceptions");
}

// 6. Key-based reconciliation (Limitation 2 fix verification)
{
  // 6a. Keyed removal: A B C D -> B C D (only 1 remove call)
  {
    const oldTree = createElement("ul", null,
      createElement("li", { key: "A" }, "A"),
      createElement("li", { key: "B" }, "B"),
      createElement("li", { key: "C" }, "C"),
      createElement("li", { key: "D" }, "D"),
    );
    const newTree = createElement("ul", null,
      createElement("li", { key: "B" }, "B"),
      createElement("li", { key: "C" }, "C"),
      createElement("li", { key: "D" }, "D"),
    );
    const { dom } = mount(oldTree);
    const bNode = dom.children[1];
    const cNode = dom.children[2];
    const dNode = dom.children[3];

    diff(dom, dom, oldTree, newTree);

    assert.equal(dom.removeCalls, 1, "only key A should be removed");
    assert.equal(dom.children.length, 3);
    assert.equal(dom.children[0], bNode);
    assert.equal(dom.children[1], cNode);
    assert.equal(dom.children[2], dNode);
  }

  // 6b. Keyed reorder: A B C -> B A C (preserves node references)
  {
    const oldTree = createElement("ul", null,
      createElement("li", { key: "A" }, "A"),
      createElement("li", { key: "B" }, "B"),
      createElement("li", { key: "C" }, "C"),
    );
    const newTree = createElement("ul", null,
      createElement("li", { key: "B" }, "B"),
      createElement("li", { key: "A" }, "A"),
      createElement("li", { key: "C" }, "C"),
    );
    const { dom } = mount(oldTree);
    const aNode = dom.children[0];
    const bNode = dom.children[1];
    const cNode = dom.children[2];

    diff(dom, dom, oldTree, newTree);

    assert.equal(dom.children.length, 3);
    assert.equal(dom.children[0], bNode);
    assert.equal(dom.children[1], aNode);
    assert.equal(dom.children[2], cNode);
  }
  console.log("PASS 6: keyed children reconciled with minimal DOM operations and reference preservation");
}

console.log("All diff tests passed.");
