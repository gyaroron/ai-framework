"use strict";

const assert = require("node:assert/strict");
const { TEXT_ELEMENT, createElement } = require("./element");

function runTests() {
  const single = createElement("div", { id: "app" });
  assert.deepEqual(single, {
    type: "div",
    props: { id: "app", children: [] },
  });
  console.log("PASS 1: single tag creates an element with empty children");

  const heading = createElement("h1", null, "Hello Mini React");
  assert.equal(heading.type, "h1");
  assert.equal(heading.props.children.length, 1);
  assert.deepEqual(heading.props.children[0], {
    type: TEXT_ELEMENT,
    props: { nodeValue: "Hello Mini React", children: [] },
  });
  console.log("PASS 2: text is wrapped as a virtual text node");

  const list = createElement(
    "ul",
    null,
    createElement("li", null, "Item 1"),
    createElement("li", null, "Item 2"),
  );
  assert.equal(list.type, "ul");
  assert.equal(list.props.children.length, 2);
  assert.equal(list.props.children[0].type, "li");
  assert.equal(list.props.children[0].props.children[0].props.nodeValue, "Item 1");
  assert.equal(list.props.children[1].type, "li");
  assert.equal(list.props.children[1].props.children[0].props.nodeValue, "Item 2");
  console.log("PASS 3: nested elements preserve multiple child levels");

  console.log("All element tests passed.");
}

runTests();
