(function () {
"use strict";

let currentInstance = null;
let hookIndex = 0;

function createHookInstance(component, props, parentDom) {
  return {
    component,
    props: props || {},
    parentDom,
    dom: null,
    vnode: null,
    renderedVNode: null,
    hooks: [],
    pendingEffects: [],
    schedule: null,
  };
}

function renderWithHooks(instance, component, props) {
  const previousInstance = currentInstance;
  const previousIndex = hookIndex;

  currentInstance = instance;
  hookIndex = 0;
  instance.pendingEffects = [];
  instance.component = component;
  instance.props = props || {};

  try {
    return component(instance.props);
  } finally {
    currentInstance = previousInstance;
    hookIndex = previousIndex;
  }
}

function flushEffects(instance) {
  const pending = instance.pendingEffects;
  instance.pendingEffects = [];
  for (const effect of pending) {
    if (typeof effect.hook.cleanup === "function") {
      effect.hook.cleanup();
    }
    const cleanup = effect.callback();
    effect.hook.cleanup = typeof cleanup === "function" ? cleanup : undefined;
    effect.hook.deps = effect.deps;
    effect.hook.initialized = true;
  }
}

function requireInstance(hookName) {
  if (currentInstance === null) {
    throw new Error(`${hookName} must be called while rendering a function component`);
  }
  return currentInstance;
}

function useState(initialValue) {
  const instance = requireInstance("useState");
  const index = hookIndex;
  hookIndex += 1;

  if (!instance.hooks[index]) {
    const hook = { state: initialValue, setter: null };
    hook.setter = function setState(action) {
      const nextState = typeof action === "function" ? action(hook.state) : action;
      if (Object.is(nextState, hook.state)) {
        return;
      }
      hook.state = nextState;
      if (typeof instance.schedule === "function") {
        instance.schedule();
      }
    };
    instance.hooks[index] = hook;
  }

  const hook = instance.hooks[index];
  return [hook.state, hook.setter];
}

function sameDependencies(previous, next) {
  if (previous === undefined || next === undefined || previous.length !== next.length) {
    return false;
  }
  for (let i = 0; i < previous.length; i += 1) {
    if (!Object.is(previous[i], next[i])) {
      return false;
    }
  }
  return true;
}

function useEffect(callback, deps) {
  const instance = requireInstance("useEffect");
  if (typeof callback !== "function") {
    throw new TypeError("useEffect expects a callback function");
  }
  if (deps !== undefined && !Array.isArray(deps)) {
    throw new TypeError("useEffect dependencies must be an array or undefined");
  }

  const index = hookIndex;
  hookIndex += 1;
  if (!instance.hooks[index]) {
    instance.hooks[index] = {
      deps: undefined,
      cleanup: undefined,
      initialized: false,
    };
  }

  const hook = instance.hooks[index];
  if (!hook.initialized || !sameDependencies(hook.deps, deps)) {
    instance.pendingEffects.push({ hook, callback, deps });
  }
}

function useMemo(factory, deps) {
  const instance = requireInstance("useMemo");
  if (typeof factory !== "function") {
    throw new TypeError("useMemo expects a factory function");
  }
  if (deps !== undefined && !Array.isArray(deps)) {
    throw new TypeError("useMemo dependencies must be an array or undefined");
  }

  const index = hookIndex;
  hookIndex += 1;
  let hook = instance.hooks[index];
  if (hook === undefined) {
    hook = { initialized: false, value: undefined, deps: undefined };
    instance.hooks[index] = hook;
  }

  // Missing deps intentionally recomputes on each render, like useEffect.
  if (!hook.initialized || deps === undefined || !sameDependencies(hook.deps, deps)) {
    hook.value = factory();
    hook.deps = deps === undefined ? undefined : deps.slice();
    hook.initialized = true;
  }
  return hook.value;
}

const api = {
  createHookInstance,
  renderWithHooks,
  flushEffects,
  useState,
  useEffect,
  useMemo,
};
if (typeof module !== "undefined" && module.exports) {
  module.exports = api;
}
if (typeof globalThis !== "undefined") {
  globalThis.MiniReactHook = api;
}
})();
