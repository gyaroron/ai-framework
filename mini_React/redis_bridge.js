"use strict";

const net = require("node:net");

function encodeCommand(args) {
  const frames = [Buffer.from(`*${args.length}\r\n`, "ascii")];
  for (const argument of args) {
    const value = Buffer.isBuffer(argument) ? argument : Buffer.from(String(argument), "utf8");
    frames.push(Buffer.from(`$${value.length}\r\n`, "ascii"), value,
      Buffer.from("\r\n", "ascii"));
  }
  return Buffer.concat(frames);
}

function findCrlf(buffer, start) {
  for (let i = start; i + 1 < buffer.length; i += 1) {
    if (buffer[i] === 13 && buffer[i + 1] === 10) return i;
  }
  return -1;
}

function decodeReply(buffer) {
  if (buffer.length === 0) return null;
  const marker = String.fromCharCode(buffer[0]);
  const lineEnd = findCrlf(buffer, 1);
  if (lineEnd === -1) return null;

  const line = buffer.toString("utf8", 1, lineEnd);
  if (marker === "+") return { value: line, consumed: lineEnd + 2 };
  if (marker === "-") {
    const error = new Error(line);
    error.respError = true;
    throw error;
  }
  if (marker === ":") {
    const integer = Number(line);
    if (!Number.isSafeInteger(integer)) throw new Error("Invalid RESP integer reply");
    return { value: integer, consumed: lineEnd + 2 };
  }
  if (marker === "$") {
    const length = Number(line);
    if (!Number.isInteger(length) || length < -1) throw new Error("Invalid RESP bulk length");
    if (length === -1) return { value: null, consumed: lineEnd + 2 };
    const dataStart = lineEnd + 2;
    const frameEnd = dataStart + length;
    if (buffer.length < frameEnd + 2) return null;
    if (buffer[frameEnd] !== 13 || buffer[frameEnd + 1] !== 10) {
      throw new Error("Invalid RESP bulk terminator");
    }
    return { value: buffer.toString("utf8", dataStart, frameEnd), consumed: frameEnd + 2 };
  }
  throw new Error(`Unsupported RESP reply marker: ${marker}`);
}

function createRedisBridge(options = {}) {
  const host = options.host || "127.0.0.1";
  const port = options.port === undefined ? 6379 : options.port;
  const timeoutMs = options.timeoutMs === undefined ? 1000 : options.timeoutMs;
  const allowFallback = options.fallback !== false;
  const mockMode = options.mock === true;
  const store = new Map();

  function mockCommand(args) {
    const [command, ...parameters] = args;
    switch (String(command).toUpperCase()) {
      case "PING": return "PONG";
      case "SET":
        if (parameters.length !== 2) throw new Error("ERR wrong number of arguments for SET");
        store.set(String(parameters[0]), String(parameters[1]));
        return "OK";
      case "GET":
        if (parameters.length !== 1) throw new Error("ERR wrong number of arguments for GET");
        return store.has(String(parameters[0])) ? store.get(String(parameters[0])) : null;
      case "DEL":
        if (parameters.length !== 1) throw new Error("ERR wrong number of arguments for DEL");
        return store.delete(String(parameters[0])) ? 1 : 0;
      default: throw new Error(`ERR unsupported command: ${command}`);
    }
  }

  function request(args) {
    if (mockMode) return Promise.resolve().then(() => mockCommand(args));

    return new Promise((resolve, reject) => {
      const socket = net.createConnection({ host, port });
      let connected = false;
      let settled = false;
      let response = Buffer.alloc(0);

      const finish = (error, value) => {
        if (settled) return;
        settled = true;
        socket.destroy();
        if (error) reject(error);
        else resolve(value);
      };

      socket.setTimeout(timeoutMs, () => {
        const error = new Error(`Mini Redis request timed out after ${timeoutMs} ms`);
        socket.destroy(error);
      });
      socket.once("connect", () => {
        connected = true;
        socket.write(encodeCommand(args));
      });
      socket.on("data", (chunk) => {
        response = Buffer.concat([response, chunk]);
        try {
          const decoded = decodeReply(response);
          if (decoded !== null) finish(null, decoded.value);
        } catch (error) {
          finish(error);
        }
      });
      socket.once("error", (error) => {
        if (!connected && allowFallback) {
          Promise.resolve().then(() => mockCommand(args)).then(
            (value) => finish(null, value),
            (mockError) => finish(mockError),
          );
        } else {
          finish(error);
        }
      });
      socket.once("end", () => {
        if (!settled) finish(new Error("Mini Redis closed before a complete RESP reply"));
      });
    });
  }

  return {
    ping: () => request(["PING"]),
    set: (key, value) => request(["SET", key, value]),
    get: (key) => request(["GET", key]),
    del: (key) => request(["DEL", key]),
    close() {},
    get mockMode() { return mockMode; },
  };
}

module.exports = { createRedisBridge, encodeCommand, decodeReply };
