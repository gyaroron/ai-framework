(function () {
"use strict";

const renderApi = typeof module !== "undefined" && module.exports
  ? require("./render")
  : globalThis.MiniReactRender;
const TEXT_ELEMENT = renderApi.TEXT_ELEMENT;
const render = renderApi.render;
const setDomProp = renderApi.setDomProp;
// render.js가 붙인 것과 같은 "batch로 감싼 handler"를 써야 뗄 때 짝이 맞는다.
const toBatchedListener = renderApi.toBatchedListener;

function isEventProp(name) {
  return name.slice(0, 2).toLowerCase() === "on";
}

function eventName(name) {
  return name.slice(2).toLowerCase();
}

function removeProp(dom, name, value) {
  if (isEventProp(name)) {
    if (typeof value === "function") {
      dom.removeEventListener(eventName(name), toBatchedListener(value));
    }
    return;
  }

  if (typeof dom.removeAttribute === "function") {
    dom.removeAttribute(name === "className" ? "class" : name);
  }
  if (name in dom) {
    dom[name] = typeof dom[name] === "boolean" ? false : "";
  } else {
    delete dom[name];
  }
}

function setProp(dom, name, value, previousValue) {
  if (isEventProp(name)) {
    if (typeof previousValue === "function") {
      dom.removeEventListener(eventName(name), toBatchedListener(previousValue));
    }
    if (typeof value === "function") {
      dom.addEventListener(eventName(name), toBatchedListener(value));
    }
    return;
  }
  setDomProp(dom, name, value);
}

function updateDom(dom, prevProps, nextProps) {
  prevProps = prevProps || {};
  nextProps = nextProps || {};

  for (const name of Object.keys(prevProps)) {
    if (name !== "children" && name !== "key" && !Object.prototype.hasOwnProperty.call(nextProps, name)) {
      removeProp(dom, name, prevProps[name]);
    }
  }

  for (const name of Object.keys(nextProps)) {
    if (name === "children" || name === "key") {
      continue;
    }
    const oldValue = prevProps[name];
    const newValue = nextProps[name];
    if (!Object.is(oldValue, newValue)) {
      setProp(dom, name, newValue, oldValue);
    }
  }
}

function childNodes(dom) {
  return dom.childNodes || dom.children || [];
}

function unmount(vnode) {
  const components = typeof module !== "undefined" && module.exports
    ? require("./component") : globalThis.MiniReactComponent;
  if (components) components.unmount(vnode);
}

function insertBefore(parent, newChild, referenceChild) {
  if (typeof parent.insertBefore === "function") {
    return parent.insertBefore(newChild, referenceChild);
  }
  if (referenceChild == null) {
    return parent.appendChild(newChild);
  }
  if (Array.isArray(parent.children)) {
    const idx = parent.children.indexOf(referenceChild);
    if (idx !== -1) {
      const existing = parent.children.indexOf(newChild);
      if (existing !== -1) parent.children.splice(existing, 1);
      const targetIdx = parent.children.indexOf(referenceChild);
      parent.children.splice(targetIdx, 0, newChild);
      return newChild;
    }
  }
  return parent.appendChild(newChild);
}

function hasKey(child) {
  return child != null && child.props != null && child.props.key != null;
}

function getKey(child, fallback) {
  if (child != null && child.props != null && child.props.key != null) {
    return String(child.props.key);
  }
  return fallback !== undefined ? fallback : null;
}

function validateKeys(children) {
  const keys = new Set();
  for (const child of children || []) {
    if (hasKey(child)) {
      const key = getKey(child);
      if (keys.has(key)) throw new Error(`Duplicate sibling key: ${key}`);
      keys.add(key);
    }
    validateKeys(child && child.props && child.props.children);
  }
}

function diffChildren(parentDom, oldChildren, newChildren) {
  oldChildren = oldChildren || [];
  newChildren = newChildren || [];

  validateKeys(oldChildren);
  validateKeys(newChildren);
  const anyHasKey = oldChildren.some(hasKey) || newChildren.some(hasKey);

  if (anyHasKey) {
    const domChildren = Array.from(childNodes(parentDom));
    const oldKeyMap = new Map();
    for (let i = 0; i < oldChildren.length; i += 1) {
      const child = oldChildren[i];
      const key = getKey(child, i);
      oldKeyMap.set(key, { vnode: child, dom: domChildren[i], index: i });
    }

    const usedKeys = new Set();
    const newKeyList = [];
    for (let i = 0; i < newChildren.length; i += 1) {
      const child = newChildren[i];
      const key = getKey(child, i);
      newKeyList.push(key);
      if (oldKeyMap.has(key)) {
        usedKeys.add(key);
      }
    }

    // 1. Remove unused old children first
    for (const [key, entry] of oldKeyMap) {
      if (!usedKeys.has(key)) {
        unmount(entry.vnode);
        if (entry.dom && (entry.dom.parentNode === parentDom || (parentDom.children && parentDom.children.includes(entry.dom)))) {
          parentDom.removeChild(entry.dom);
        }
      }
    }

    // 2. Reconcile and position new children
    for (let i = 0; i < newChildren.length; i += 1) {
      const newChild = newChildren[i];
      const key = newKeyList[i];
      const currentLiveChildren = Array.from(childNodes(parentDom));
      let targetReference = currentLiveChildren[i] || null;

      if (oldKeyMap.has(key)) {
        const entry = oldKeyMap.get(key);
        const updatedDom = diff(parentDom, entry.dom, entry.vnode, newChild);
        targetReference = Array.from(childNodes(parentDom))[i] || null;
        if (updatedDom !== targetReference) {
          insertBefore(parentDom, updatedDom, targetReference);
        }
      } else {
        const temporaryContainer = document.createDocumentFragment
          ? document.createDocumentFragment()
          : { children: [], appendChild(node) { this.children.push(node); return node; } };
        const newDom = render(newChild, temporaryContainer);
        insertBefore(parentDom, newDom, targetReference);
      }
    }
    return;
  }

  // Keyless positional reconciliation
  const domChildren = Array.from(childNodes(parentDom));
  const commonLength = Math.min(oldChildren.length, newChildren.length);

  for (let i = 0; i < commonLength; i += 1) {
    diff(parentDom, domChildren[i], oldChildren[i], newChildren[i]);
  }

  if (newChildren.length > oldChildren.length) {
    for (let i = commonLength; i < newChildren.length; i += 1) {
      diff(parentDom, null, null, newChildren[i]);
    }
  }

  if (oldChildren.length > newChildren.length) {
    for (let i = oldChildren.length - 1; i >= commonLength; i -= 1) {
      diff(parentDom, domChildren[i], oldChildren[i], null);
    }
  }
}

function diff(parentDom, currentDom, oldVNode, newVNode) {
  validateKeys(oldVNode ? [oldVNode] : []);
  validateKeys(newVNode ? [newVNode] : []);
  if (oldVNode == null && newVNode == null) {
    return null;
  }
  if (oldVNode == null) {
    return render(newVNode, parentDom);
  }
  if (newVNode == null) {
    unmount(oldVNode);
    parentDom.removeChild(currentDom);
    return null;
  }

  if (typeof oldVNode.type === "function" &&
      typeof newVNode.type === "function" &&
      oldVNode.type === newVNode.type) {
    const componentApi = typeof module !== "undefined" && module.exports
      ? require("./component")
      : globalThis.MiniReactComponent;
    return componentApi.updateComponent(parentDom, currentDom, oldVNode, newVNode, diff);
  }

  if (oldVNode.type !== newVNode.type) {
    const temporaryContainer = document.createDocumentFragment
      ? document.createDocumentFragment()
      : { children: [], appendChild(node) { this.children.push(node); return node; } };
    const newDom = render(newVNode, temporaryContainer);
    unmount(oldVNode);
    parentDom.replaceChild(newDom, currentDom);
    return newDom;
  }

  updateDom(currentDom, oldVNode.props, newVNode.props);
  const oldChildren = (oldVNode.props && oldVNode.props.children) || [];
  const newChildren = (newVNode.props && newVNode.props.children) || [];
  diffChildren(currentDom, oldChildren, newChildren);
  return currentDom;
}

const api = { diff, diffChildren, updateDom };
if (typeof module !== "undefined" && module.exports) {
  module.exports = api;
}
if (typeof globalThis !== "undefined") {
  globalThis.MiniReactDiff = api;
}
})();
