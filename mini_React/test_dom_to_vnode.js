"use strict";

const assert = require("node:assert/strict");
const { TEXT_ELEMENT, domToVnode, domToVnodeList } = require("./element");

function element(name, attributes = [], childNodes = []) {
  return {
    nodeType: 1,
    localName: name,
    attributes: attributes.map(([attrName, value]) => ({
      name: attrName,
      value,
    })),
    childNodes,
  };
}

function text(value, nodeType = 3) {
  return { nodeType, nodeValue: value };
}

const tree = element("main", [["class", "app"], ["data-mode", "demo"]], [
  text("\n  "),
  element("label", [["for", "name"], ["onclick", "alert(1)"]], [
    text("Name"),
    element("input", [["id", "name"], ["checked", ""]]),
  ]),
  text("\n  "),
  { nodeType: 8, nodeValue: "ignored comment" },
  element("script", [], [text("window.bad = true")]),
]);

const vnode = domToVnode(tree);
assert.deepEqual(vnode, {
  type: "main",
  props: {
    className: "app",
    "data-mode": "demo",
    children: [{
      type: "label",
      props: {
        htmlFor: "name",
        children: [
          { type: TEXT_ELEMENT, props: { nodeValue: "Name", children: [] } },
          {
            type: "input",
            props: { id: "name", checked: true, children: [] },
          },
        ],
      },
    }],
  },
});
assert.equal(domToVnode(text("  \n \t")), null);
assert.equal(domToVnode(text("plain text")).props.nodeValue, "plain text");
assert.equal(domToVnode(element("script", [], [])), null);
assert.equal(domToVnode(tree, { sanitize: false }).props.children.length, 2);

// Key and data-key attribute parsing
{
  const nodeWithKey = element("li", [["key", "item-1"], ["class", "item"]], [text("A")]);
  const nodeWithDataKey = element("li", [["data-key", "item-2"]], [text("B")]);
  assert.equal(domToVnode(nodeWithKey).props.key, "item-1");
  assert.equal(domToVnode(nodeWithDataKey).props.key, "item-2");
}

// domToVnodeList tests (Limitation 1 fix verification)
{
  const container = element("div", [], [
    element("article", [], [text("First")]),
    element("p", [], [text("Second")]),
  ]);
  const list = domToVnodeList(container);
  assert.equal(list.length, 2);
  assert.equal(list[0].type, "article");
  assert.equal(list[1].type, "p");

  const emptyContainer = element("div", [], []);
  assert.deepEqual(domToVnodeList(emptyContainer), []);
}

console.log("PASS: DOM elements and text became VNodes; formatting whitespace, comments, scripts, and inline handlers followed the conversion policy.");
console.log("PASS: domToVnodeList and key attribute extraction verified.");
console.log("All DOM-to-VNode tests passed.");
