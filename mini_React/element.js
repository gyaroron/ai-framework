(function () {
"use strict";

const TEXT_ELEMENT = "TEXT_ELEMENT";

function createTextElement(text) {
  return {
    type: TEXT_ELEMENT,
    props: {
      nodeValue: String(text),
      children: [],
    },
  };
}

function normalizeChild(child) {
  if (typeof child === "string" || typeof child === "number") {
    return createTextElement(child);
  }
  return child;
}

function createElement(type, props, ...children) {
  const normalizedChildren = children
    .flat(Infinity)
    .filter((child) => child !== null && child !== undefined && child !== false)
    .map(normalizeChild);

  return {
    type,
    props: {
      ...(props || {}),
      children: normalizedChildren,
    },
  };
}

function isFormattingWhitespace(text) {
  return /^\s*$/.test(text) && text.includes("\n");
}

function domToVnode(domNode, options = {}) {
  const sanitize = options.sanitize !== false;
  if (domNode == null) return null;

  if (domNode.nodeType === 3) {
    const text = domNode.nodeValue == null ? "" : String(domNode.nodeValue);
    return isFormattingWhitespace(text) ? null : createTextElement(text);
  }
  if (domNode.nodeType !== 1) return null;

  const type = String(domNode.localName || domNode.tagName || "").toLowerCase();
  if (type === "" || (sanitize && type === "script")) return null;

  const props = {};
  const booleanAttributes = new Set([
    "checked", "selected", "disabled", "multiple", "readonly", "required",
    "autofocus", "hidden", "open", "novalidate", "formnovalidate",
  ]);
  for (const attribute of Array.from(domNode.attributes || [])) {
    const sourceName = String(attribute.name);
    if (sanitize && /^on/i.test(sourceName)) continue;
    const name = sourceName.toLowerCase() === "class"
      ? "className"
      : sourceName.toLowerCase() === "for" ? "htmlFor" : sourceName;
    props[name] = booleanAttributes.has(sourceName.toLowerCase())
      ? true
      : String(attribute.value);
  }

  if (props.key === undefined && props["data-key"] !== undefined) {
    props.key = props["data-key"];
  }

  const source = type === "template" && domNode.content
    ? domNode.content
    : domNode;
  const children = [];
  for (const child of Array.from(source.childNodes || [])) {
    const vnode = domToVnode(child, options);
    if (vnode !== null) children.push(vnode);
  }
  return { type, props: { ...props, children } };
}

function domToVnodeList(container, options = {}) {
  if (container == null) return [];
  const list = [];
  const source = container.childNodes || container.children || [];
  for (const child of Array.from(source)) {
    const vnode = domToVnode(child, options);
    if (vnode !== null) {
      list.push(vnode);
    }
  }
  return list;
}

function normalizeDom(domNode, options = {}) {
  const sanitize = options.sanitize !== false;
  for (const child of Array.from(domNode.childNodes || [])) {
    if (child.nodeType === 3) {
      if (isFormattingWhitespace(String(child.nodeValue || ""))) {
        child.parentNode.removeChild(child);
      }
    } else if (child.nodeType === 1) {
      if (sanitize && String(child.localName || child.tagName).toLowerCase() === "script") {
        child.parentNode.removeChild(child);
        continue;
      }
      if (sanitize) {
        for (const attribute of Array.from(child.attributes || [])) {
          if (/^on/i.test(attribute.name)) child.removeAttribute(attribute.name);
        }
      }
      normalizeDom(child, options);
    } else if (child.nodeType !== 1) {
      child.parentNode.removeChild(child);
    }
  }
  if (typeof domNode.normalize === "function") domNode.normalize();
  return domNode;
}

const api = {
  TEXT_ELEMENT,
  createElement,
  createTextElement,
  domToVnode,
  domToVnodeList,
  normalizeDom,
};

if (typeof module !== "undefined" && module.exports) module.exports = api;
if (typeof globalThis !== "undefined") globalThis.MiniReactElement = api;
})();
