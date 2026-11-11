"use strict";

/*
 * 이 폴더의 Node.js 테스트 · 성능 비교 script가 쓰는 작은 RESP client.
 * client 하나가 TCP 연결 하나를 계속 쓰고, 보낸 순서대로 응답을 짝지어 돌려준다.
 */
const net = require("node:net");

const CRLF = "\r\n";

function encodeCommand(args) {
  const parts = [`*${args.length}${CRLF}`];
  for (const arg of args) {
    const text = String(arg);
    parts.push(`$${Buffer.byteLength(text)}${CRLF}${text}${CRLF}`);
  }
  return parts.join("");
}

/* 완성된 응답 하나를 { value, consumed }로 돌려준다. byte가 더 와야 하면 null. */
function decodeReply(buffer) {
  const lineEnd = buffer.indexOf(CRLF);
  if (lineEnd < 0) return null;
  const type = String.fromCharCode(buffer[0]);
  const line = buffer.toString("utf8", 1, lineEnd);
  const afterLine = lineEnd + 2;

  if (type === "+") return { value: line, consumed: afterLine };
  if (type === ":") return { value: Number(line), consumed: afterLine };
  if (type === "-") {
    const error = new Error(line);
    error.isRespError = true;
    return { value: error, consumed: afterLine };
  }
  if (type === "$") {
    const length = Number(line);
    if (length < 0) return { value: null, consumed: afterLine };
    if (buffer.length < afterLine + length + 2) return null;
    return {
      value: buffer.toString("utf8", afterLine, afterLine + length),
      consumed: afterLine + length + 2,
    };
  }
  throw new Error(`Unsupported RESP reply type: ${type}`);
}

function connect(options = {}) {
  const host = options.host || "127.0.0.1";
  const port = options.port;

  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ host, port });
    const waiting = [];
    let pending = Buffer.alloc(0);
    // 연결이 끊긴 이유. null이 아니면 그 뒤의 명령은 보내지 않고 바로 실패로 답한다.
    let closedError = null;

    function failAll(error) {
      closedError = closedError || error;
      while (waiting.length > 0) waiting.shift().reject(error);
    }

    socket.setNoDelay(true);
    socket.once("error", reject);
    socket.once("connect", () => {
      socket.removeListener("error", reject);
      socket.on("error", failAll);
      socket.on("close", () => failAll(new Error("RESP connection closed")));
      socket.on("data", (chunk) => {
        pending = Buffer.concat([pending, chunk]);
        while (waiting.length > 0) {
          let decoded;
          try {
            decoded = decodeReply(pending);
          } catch (error) {
            failAll(error);
            socket.destroy();
            return;
          }
          if (decoded === null) return;
          pending = pending.subarray(decoded.consumed);
          const next = waiting.shift();
          if (decoded.value instanceof Error) next.reject(decoded.value);
          else next.resolve(decoded.value);
        }
      });

      resolve({
        command(args) {
          return new Promise((resolveReply, rejectReply) => {
            // 끊긴 연결에 쓰면 응답이 영원히 오지 않으므로 기다리지 않고 실패시킨다.
            if (closedError !== null) {
              rejectReply(closedError);
              return;
            }
            waiting.push({ resolve: resolveReply, reject: rejectReply });
            socket.write(encodeCommand(args));
          });
        },
        close() {
          closedError = closedError || new Error("RESP connection closed");
          socket.destroy();
        },
      });
    });
  });
}

module.exports = { connect, encodeCommand, decodeReply };
