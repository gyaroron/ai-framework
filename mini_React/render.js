(function () {
"use strict";

const TEXT_ELEMENT = "TEXT_ELEMENT";

function setDomProp(dom, name, value) {
  const isAttribute = name.slice(0, 5) === "data-" ||
    name.slice(0, 5) === "aria-" || !(name in dom);
  if (isAttribute && typeof dom.setAttribute === "function") {
    dom.setAttribute(name, String(value));
  } else {
    dom[name] = value;
  }
}

// hook.js의 toBatchedListener를 찾는다. render.js가 hook.js보다 먼저 쓰일 수 있어 부를 때 찾는다.
function toBatchedListener(handler) {
  const hookApi = typeof module !== "undefined" && module.exports
    ? require("./hook")
    : globalThis.MiniReactHook;
  return hookApi.toBatchedListener(handler);
}

function render(element, container) {
  if (element == null || typeof element !== "object") {
    throw new TypeError("render expects a virtual DOM element object");
  }
  if (container == null || typeof container.appendChild !== "function") {
    throw new TypeError("render expects a DOM container with appendChild()");
  }

  if (typeof element.type === "function") {
    const componentApi = typeof module !== "undefined" && module.exports
      ? require("./component")
      : globalThis.MiniReactComponent;
    return componentApi.renderComponent(element, container, render);
  }

  const isTextElement = element.type === TEXT_ELEMENT;
  const dom = isTextElement
    ? document.createTextNode("")
    : document.createElement(element.type);

  for (const name of Object.keys(element.props || {})) {
    if (name === "children" || name === "key") {
      continue;
    }
    if (name.slice(0, 2).toLowerCase() === "on" &&
        typeof element.props[name] === "function") {
      dom.addEventListener(name.slice(2).toLowerCase(),
        toBatchedListener(element.props[name]));
      continue;
    }
    if (isTextElement && name === "nodeValue") {
      dom.nodeValue = element.props[name];
    } else {
      setDomProp(dom, name, element.props[name]);
    }
  }

  const children = (element.props && element.props.children) || [];
  for (const child of children) {
    render(child, dom);
  }

  container.appendChild(dom);
  return dom;
}

const api = { TEXT_ELEMENT, render, setDomProp, toBatchedListener };
if (typeof module !== "undefined" && module.exports) {
  module.exports = api;
}
if (typeof globalThis !== "undefined") {
  globalThis.MiniReactRender = api;
}
})();
