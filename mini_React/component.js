(function () {
"use strict";

const componentInstances = new WeakMap();
let activeRenderDepth = 0;

function runInsideComponent(work) {
  activeRenderDepth += 1;
  try {
    return work();
  } finally {
    activeRenderDepth -= 1;
  }
}

function hookApi() {
  return typeof module !== "undefined" && module.exports
    ? require("./hook")
    : globalThis.MiniReactHook;
}

function setInstance(element, instance) {
  componentInstances.set(element, instance);
}

function getComponentInstance(element) {
  return componentInstances.get(element);
}

function renderApi() {
  return typeof module !== "undefined" && module.exports
    ? require("./render")
    : globalThis.MiniReactRender;
}

function diffApi() {
  return typeof module !== "undefined" && module.exports
    ? require("./diff")
    : globalThis.MiniReactDiff;
}

class FunctionComponent {
  constructor(component, props = {}) {
    if (typeof component !== "function") {
      throw new TypeError("FunctionComponent requires a function");
    }
    this.component = component;
    this.props = props || {};
    this.parentDom = null;
    this.dom = null;
    this.vnode = null;
    this.renderedVNode = null;
    this.hooks = [];
    this.isRoot = activeRenderDepth === 0;
    this._hookInstance = hookApi().createHookInstance(component, this.props, null);
    this._hookInstance.hooks = this.hooks;
    this._hookInstance.isRoot = this.isRoot;
    this._hookInstance.schedule = () => this._scheduleUpdate();
  }

  mount(parentDom, renderFn) {
    if (parentDom == null) throw new TypeError("mount requires a parent DOM node");
    this.parentDom = parentDom;
    this._hookInstance.parentDom = parentDom;
    const hooks = hookApi();
    this.renderedVNode = hooks.renderWithHooks(
      this._hookInstance, this.component, this.props,
    );
    this.dom = runInsideComponent(
      () => (renderFn || renderApi().render)(this.renderedVNode, parentDom),
    );
    hooks.flushEffects(this._hookInstance);
    return this.dom;
  }

  update(nextProps = this.props, parentDom = this.parentDom,
         currentDom = this.dom, diffFn) {
    if (parentDom == null || currentDom == null || this.renderedVNode == null) {
      throw new Error("update requires a mounted component");
    }
    const hooks = hookApi();
    const nextPropsValue = nextProps || {};
    const nextRendered = hooks.renderWithHooks(
      this._hookInstance, this.component, nextPropsValue,
    );
    this.dom = runInsideComponent(() => (diffFn || diffApi().diff)(
      parentDom, currentDom, this.renderedVNode, nextRendered,
    ));
    this.parentDom = parentDom;
    this.props = nextPropsValue;
    this.vnode = { type: this.component, props: this.props };
    this.renderedVNode = nextRendered;
    hooks.flushEffects(this._hookInstance);
    return this.dom;
  }

  _scheduleUpdate() {
    if (this.dom === null || this.renderedVNode === null) return;
    this.update(this.props);
  }
}

function unmount(element) {
  if (element == null) return;
  if (typeof element.type === "function") {
    const instance = getComponentInstance(element);
    if (!instance || instance.renderedVNode === null) return;
    const rendered = instance.renderedVNode;
    // Disable updates before invoking user cleanup, which can call setters.
    instance.dom = null;
    instance.renderedVNode = null;
    instance.parentDom = null;
    instance._hookInstance.schedule = null;
    instance._hookInstance.pendingEffects = [];
    unmount(rendered);
    for (const hook of instance.hooks) {
      const cleanup = hook.cleanup;
      hook.cleanup = undefined;
      if (typeof cleanup === "function") cleanup();
    }
    return;
  }
  for (const child of (element.props && element.props.children) || []) unmount(child);
}

function renderComponent(element, container, render) {
  if (typeof element.type !== "function") {
    throw new TypeError("renderComponent expects a function component VNode");
  }
  const instance = new FunctionComponent(element.type, element.props || {});
  instance.vnode = element;
  const dom = instance.mount(container, render);
  setInstance(element, instance);
  return dom;
}

function updateComponent(parentDom, currentDom, oldElement, newElement, diff) {
  const hooks = hookApi();
  let instance = getComponentInstance(oldElement);

  if (instance === undefined) {
    instance = new FunctionComponent(oldElement.type, oldElement.props || {});
    instance.parentDom = parentDom;
    instance.dom = currentDom;
    instance.renderedVNode = hooks.renderWithHooks(
      instance._hookInstance, oldElement.type, oldElement.props || {},
    );
    hooks.flushEffects(instance._hookInstance);
  }

  instance.vnode = newElement;
  const updatedDom = instance.update(
    newElement.props || {}, parentDom, currentDom, diff,
  );
  setInstance(newElement, instance);
  return updatedDom;
}

const api = { FunctionComponent, renderComponent, updateComponent, getComponentInstance, unmount };
if (typeof module !== "undefined" && module.exports) {
  module.exports = api;
}
if (typeof globalThis !== "undefined") {
  globalThis.MiniReactComponent = api;
}
})();
