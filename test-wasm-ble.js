#!/usr/bin/env node
// test-wasm-ble.js — WASM BLE E2E without external infra.
//
// Spins a minimal in-process WebSocket HCI server (answers the demo's
// bring-up commands with Command Complete), boots the RV32 BLE demo
// (web/wifi_ble_adv_rv32.uf2) in Bramble WASM via web/cli.js pointed at
// it with --ble-hci, and asserts ADV-OK on UART. Covers: WASM CYW43 BT
// shared bus, H4 ring uplink (pop/push exports), cli.js WS plumbing.
// Usage: node test-wasm-ble.js  (exit 0 = PASS)
import http from 'http';
import crypto from 'crypto';
import { spawn } from 'child_process';

// ---- minimal WS server (accepts Upgrade, binary frames both ways) ----
function startHci() {
  const server = http.createServer();
  const seen = { cmds: [] };
  server.on('upgrade', (req, sock) => {
    const key = req.headers['sec-websocket-key'] || '';
    const acc = crypto.createHash('sha1')
      .update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    sock.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n' +
      'Connection: Upgrade\r\nSec-WebSocket-Accept: ' + acc + '\r\n\r\n');
    let buf = Buffer.alloc(0);
    sock.on('data', (d) => { buf = Buffer.concat([buf, d]); pump(); });
    function send(frame) {
      frame = Buffer.from(frame);
      let h;
      if (frame.length < 126) {
        h = Buffer.alloc(2); h[0] = 0x82; h[1] = frame.length;
      } else {
        h = Buffer.alloc(4); h[0] = 0x82; h[1] = 126;
        h.writeUInt16BE(frame.length, 2);
      }
      sock.write(Buffer.concat([h, frame]));
    }
    function h4len(b) {
      if (b.length < 1) return 0;
      const t = b[0];
      if (t === 1 && b.length >= 4) return 4 + b[3];
      if (t === 4 && b.length >= 3) return 3 + b[2];
      if (t === 2 && b.length >= 5) return 5 + (b[3] | (b[4] << 8));
      if (t === 3 && b.length >= 4) return 4 + b[3];
      if (t === 5 && b.length >= 5) return 5 + (b[3] | ((b[4] & 0x3f) << 8));
      if (t >= 1 && t <= 5) return 0;
      return -1;
    }
    function pump() {
      for (;;) {
        if (buf.length < 2) return;
        const b0 = buf[0], b1 = buf[1];
        if ((b0 & 0x0f) !== 0x02) { buf = buf.slice(1); continue; }
        const masked = (b1 & 0x80) !== 0;
        let len = b1 & 0x7f, off = 2;
        if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
        else if (len === 127) return;
        const mlen = masked ? 4 : 0;
        if (buf.length < off + mlen + len) return;
        let payload = buf.slice(off + mlen, off + mlen + len);
        if (masked) {
          const m = buf.slice(off, off + 4);
          payload = Buffer.from(payload.map((v, i) => v ^ m[i % 4]));
        }
        buf = buf.slice(off + mlen + len);
        onH4(payload);
      }
    }
    function onH4(pkt) {
      if (pkt.length < 4 || pkt[0] !== 1) return;
      const op = pkt[1] | (pkt[2] << 8);
      seen.cmds.push(op);
      // Command Complete (status 0), correct length: params = ncmd+op+status.
      const cc = Buffer.from([0x04, 0x0E, 0x04, 0x01, pkt[1], pkt[2], 0x00]);
      send(cc);
    }
  });
  return new Promise((res) => server.listen(0, '127.0.0.1', () => res({ server, port: server.address().port, seen })));
}

// ---- boot cli.js, wait for ADV-OK on UART ----
const { server, port, seen } = await startHci();
console.log(`[test] fake HCI controller on 127.0.0.1:${port}`);

const child = spawn('node', ['web/cli.js', 'web/wifi_ble_adv_rv32.uf2',
  '--arch', 'rv32', '--steps', '1500000000', '--timeout', '600',
  '--wifi', '--ble-hci', `ws://127.0.0.1:${port}/ble`],
  { stdio: ['ignore', 'pipe', 'inherit'] });

let out = '';
child.stdout.on('data', (d) => { out += d.toString(); });
const deadline = Date.now() + 590000;
while (Date.now() < deadline) {
  await new Promise((r) => setTimeout(r, 3000));
  if (/RV32 BLE (ADV-OK|CC-FAIL|SCAN-TIMEOUT)/.test(out)) break;
}
console.log('[test] uart tail:', JSON.stringify(out.slice(-250)));
console.log('[test] opcodes seen:', seen.cmds.map((o) => '0x' + o.toString(16)).join(' '));
if (/RV32 BLE ADV-OK/.test(out))
  console.log('[test] WASM BLE E2E PASS');
else { console.log('[test] WASM BLE E2E FAIL (no ADV-OK)'); process.exitCode = 1; }
child.kill();
server.close();
