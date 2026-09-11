#!/usr/bin/env node
// test-wasm-gateway.js — WASM WiFi E2E without external infra.
//
// Spins a minimal in-process WebSocket gateway (DHCP + ARP, stdlib only),
// boots the RV32 join demo (web/wifi_join_rv32.uf2: joins BrambleNet via
// WPA2, DHCP) in Bramble WASM via web/cli.js pointed at it, and asserts
// the lease. Covers: WASM CYW43 model, vnet mirror, cli.js WS plumbing.
// Usage: node test-wasm-gateway.js  (exit 0 = PASS)
import http from 'http';
import crypto from 'crypto';
import { spawn } from 'child_process';

const GW_MAC = [0x02, 0x12, 0x34, 0x56, 0x78, 0x01];
const GW_IP = [192, 168, 4, 1];
const CLI_IP = [192, 168, 4, 2];
const MASK = [255, 255, 255, 0];

function csum(data) {
  let s = 0;
  for (let i = 0; i + 1 < data.length; i += 2) s += (data[i] << 8) + data[i + 1];
  if (data.length & 1) s += data[data.length - 1] << 8;
  while (s >> 16) s = (s & 0xffff) + (s >>> 16);
  return (~s) & 0xffff;
}
const B = (...xs) => Buffer.from(xs);

// ---- minimal WS server (accepts Upgrade, binary frames both ways) ----
function startGateway() {
  const server = http.createServer();
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
        onEth(payload);
      }
    }
    function onEth(f) {
      if (f.length < 14) return;
      if (f[12] === 0x08 && f[13] === 0x06 && f.length >= 42) return arp(f);
      if (f[12] === 0x08 && f[13] === 0x00 && f.length >= 14 + 20 + 8) {
        const ip = f.slice(14);
        if (ip[9] === 17 && (ip[22] << 8) + ip[23] === 67) return dhcp(f);
      }
    }
    function arp(f) {
      if (f[20] !== 0 || f[21] !== 1) return;
      if (f[24] !== 192 || f[25] !== 168 || f[26] !== 4 || f[28] !== GW_IP[0]) return;
      if (f[38] !== GW_IP[3]) return;
      const r = Buffer.concat([f.slice(6, 12), B(...GW_MAC), B(8, 6),
        B(0, 1, 8, 0, 6, 4, 0, 2), B(...GW_MAC), B(...GW_IP), f.slice(22, 28), f.slice(28, 32)]);
      send(r.length < 60 ? Buffer.concat([r, Buffer.alloc(60 - r.length)]) : r);
    }
    function dhcp(f) {
      const udp = f.slice(14 + 20);
      const bootp = udp.slice(8);
      if (bootp[0] !== 1 || bootp[236] !== 99) return;
      let msg = 0;
      for (let i = 240; i + 2 < bootp.length && bootp[i] !== 255;) {
        if (bootp[i] === 53) { msg = bootp[i + 2]; break; }
        i += 2 + (bootp[i + 1] || 0);
      }
      const chaddr = bootp.slice(28, 34);
      if (msg === 1) send(dhcpReply(bootp, chaddr, 2));       // OFFER
      else if (msg === 3) send(dhcpReply(bootp, chaddr, 5));  // ACK
    }
    function dhcpReply(req, chaddr, mtype) {
      const bootp = Buffer.alloc(300, 0);
      bootp[0] = 2; bootp[1] = 1; bootp[2] = 6; bootp[3] = 0;
      req.copy(bootp, 4, 4, 8);
      bootp.writeUInt16BE(0x8000, 10);
      B(...chaddr).copy(bootp, 28);
      B(...CLI_IP).copy(bootp, 16);
      B(...GW_IP).copy(bootp, 20);
      bootp[236] = 99; bootp[237] = 130; bootp[238] = 83; bootp[239] = 99;
      let o = 240;
      const opt = (c, ...v) => { bootp[o++] = c; bootp[o++] = v.length; for (const x of v) bootp[o++] = x; };
      opt(53, mtype); opt(1, ...MASK); opt(3, ...GW_IP); opt(6, ...GW_IP);
      opt(51, 0, 1, 81, 128); opt(54, ...GW_IP); bootp[o++] = 255;
      const body = bootp.slice(0, o);
      const uh = Buffer.alloc(8);
      uh.writeUInt16BE(67, 0); uh.writeUInt16BE(68, 2); uh.writeUInt16BE(8 + body.length, 4);
      const pseudo = Buffer.concat([B(...GW_IP), B(255, 255, 255, 255), B(0, 17)]);
      const plen = Buffer.alloc(2); plen.writeUInt16BE(8 + body.length, 0);
      uh.writeUInt16BE(csum(Buffer.concat([pseudo, plen, uh.slice(0, 6), B(0, 0), body])), 6);
      const ih = B(0x45, 0, 0, 0, 0x12, 0x34, 0, 0, 64, 17, 0, 0, ...GW_IP, 255, 255, 255, 255);
      ih.writeUInt16BE(20 + 8 + body.length, 2);
      ih.writeUInt16BE(csum(ih), 10);
      return Buffer.concat([B(0xff, 0xff, 0xff, 0xff, 0xff, 0xff), B(...GW_MAC), B(8, 0), ih, uh, body]);
    }
  });
  return new Promise((res) => server.listen(0, '127.0.0.1', () => res({ server, port: server.address().port })));
}

// ---- boot cli.js, wait for DONE + lease in UART ----
const { server, port } = await startGateway();
console.log(`[test] fake gateway on 127.0.0.1:${port}`);

const child = spawn('node', ['web/cli.js', 'web/wifi_join_rv32.uf2',
  '--arch', 'rv32', '--steps', '1500000000', '--timeout', '600',
  '--gateway', `ws://127.0.0.1:${port}/net`, '--room', 't1'],
  { stdio: ['ignore', 'pipe', 'inherit'] });

let out = '';
child.stdout.on('data', (d) => { out += d.toString(); });
const deadline = Date.now() + 590000;
while (Date.now() < deadline) {
  await new Promise((r) => setTimeout(r, 3000));
  if (/RV32 JOIN (DONE|FAIL|NOIP)/.test(out)) break;
}
console.log('[test] uart tail:', JSON.stringify(out.slice(-250)));
if (/RV32 JOIN IP=192\.168\.4\.2/.test(out) && /RV32 JOIN DONE/.test(out))
  console.log('[test] WASM GATEWAY E2E PASS');
else { console.log('[test] WASM GATEWAY E2E FAIL (no lease)'); process.exitCode = 1; }
child.kill();
server.close();
