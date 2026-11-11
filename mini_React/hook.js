(function () {
"use strict";

let currentInstance = null;
let hookIndex = 0;
let effectDepth = 0;
const deferredUpdates = new Set();
// batching(묶음 처리): batch() 안에서 부른 setState는 바로 다시 그리지 않고,
// 다시 그려야 할 component를 여기에 모아 두었다가 batch()가 끝날 때 한 번만 그린다.
let batchDepth = 0;
const batchedUpdates = new Set();
// event handler → batch()로 감싼 handler. 같은 handler는 항상 같은 감싼 함수를 받아야
// addEventListener로 붙인 것을 removeEventListener로 다시 뗄 수 있다.
const batchedListeners = new WeakMap();

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
    isRoot: true,
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
  effectDepth += 1;
  try {
    for (const effect of pending) {
      effect.hook.deps = effect.deps;
      effect.hook.initialized = true;
      const previousCleanup = effect.hook.cleanup;
      effect.hook.cleanup = undefined;
      if (typeof previousCleanup === "function") previousCleanup();
      const cleanup = effect.callback();
      effect.hook.cleanup = typeof cleanup === "function" ? cleanup : undefined;
    }
  } finally {
    effectDepth -= 1;
    if (effectDepth === 0) {
      while (deferredUpdates.size > 0) {
        const next = deferredUpdates.values().next().value;
        deferredUpdates.delete(next);
        if (typeof next.schedule === "function") next.schedule();
      }
    }
  }
}

// work 안에서 일어난 여러 setState를 한 번의 update로 묶는다.
// batch 안에서 batch를 다시 불러도 가장 바깥 batch가 끝날 때 한 번만 그린다.
function batch(work) {
  batchDepth += 1;
  try {
    return work();
  } finally {
    batchDepth -= 1;
    if (batchDepth === 0) {
      while (batchedUpdates.size > 0) {
        const next = batchedUpdates.values().next().value;
        batchedUpdates.delete(next);
        if (typeof next.schedule === "function") next.schedule();
      }
    }
  }
}

// DOM에 붙일 event handler를 batch()로 감싼다.
// 사용자가 한 번 클릭했을 때 handler가 setState를 여러 번 불러도 화면은 한 번만 다시 그린다.
function toBatchedListener(handler) {
  let listener = batchedListeners.get(handler);
  if (listener === undefined) {
    listener = function batchedListener(event) {
      return batch(() => handler.call(this, event));
    };
    batchedListeners.set(handler, listener);
  }
  return listener;
}

function requireInstance(hookName) {
  if (currentInstance === null) {
    throw new Error(`${hookName} must be called while rendering a function component`);
  }
  if (currentInstance.isRoot === false) {
    throw new Error(`${hookName} can only be used in the root component; `
      + "child components must stay stateless and receive props");
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
        // effect 실행 중이면 effect가 다 끝난 뒤에, batch 안이면 batch가 끝난 뒤에 한 번만 그린다.
        // 둘 다 아니면 지금 바로 다시 그린다.
        if (effectDepth > 0) deferredUpdates.add(instance);
        else if (batchDepth > 0) batchedUpdates.add(instance);
        else instance.schedule();
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
  batch,
  toBatchedListener,
};
if (typeof module !== "undefined" && module.exports) {
  module.exports = api;
}
if (typeof globalThis !== "undefined") {
  globalThis.MiniReactHook = api;
}
})();
