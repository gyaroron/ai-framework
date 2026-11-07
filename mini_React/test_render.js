"use strict";

const assert = require("node:assert/strict");
const { createElement } = require("./element");
const { render } = require("./render");

class MockNode {
  constructor(type, nodeType) {
    this.type = type;
    this.nodeType = nodeType;
    this.children = [];
    this.props = {};
    this.nodeValue = "";
  }

  appendChild(child) {
    this.children.push(child);
    return child;
  }

  set id(value) {
    this.props.id = value;
  }

  get id() {
    return this.props.id;
  }

  set className(value) {
    this.props.className = value;
  }

  get className() {
    return this.props.className;
  }

  get textContent() {
    if (this.nodeType === 3) {
      return this.nodeValue;
    }
    return this.children.map((child) => child.textContent).join("");
  }
}

global.document = {
  createElement(type) {
    return new MockNode(type, 1);
  },
  createTextNode(text) {
    const node = new MockNode("#text", 3);
    node.nodeValue = text;
    return node;
  },
};

const container = new MockNode("#container", 1);
const tree = createElement(
  "div",
  { id: "app", className: "root" },
  createElement("h1", null, "Hello Mini React"),
  createElement("ul", null,
    createElement("li", null, "Item 1"),
    createElement("li", null, "Item 2"),
  ),
);

const rendered = render(tree, container);
assert.equal(container.children.length, 1);
assert.equal(rendered.type, "div");
assert.equal(rendered.id, "app");
assert.equal(rendered.className, "root");
assert.equal(rendered.children.length, 2);
assert.equal(rendered.children[0].type, "h1");
assert.equal(rendered.children[0].children[0].nodeType, 3);
assert.equal(rendered.children[0].textContent, "Hello Mini React");
assert.equal(rendered.children[1].type, "ul");
assert.equal(rendered.children[1].children.length, 2);
assert.equal(rendered.children[1].children[0].textContent, "Item 1");
assert.equal(rendered.children[1].children[1].textContent, "Item 2");
assert.equal(Object.hasOwn(rendered.props, "children"), false);

console.log("PASS: render created the expected mock DOM tree and properties.");
console.log("All render tests passed.");
