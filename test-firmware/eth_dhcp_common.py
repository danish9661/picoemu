#!/usr/bin/env python3
"""Shared DHCP frame builder for eth_dhcp guest firmware (all arches).

Builds the exact byte blobs the guest embeds in flash:
  DISCOVER (342B) and REQUEST (354B) Ethernet frames, plus expected
  parse offsets for OFFER/ACK. MAC/XID parameterized per arch so the
  three sweep runs don't cross-talk on a shared gateway room.
"""

BROADCAST = bytes([0xFF] * 6)


def build_discover(mac, xid):
    """DHCP DISCOVER Ethernet frame (342B: 14 ETH + 20 IP + 8 UDP + 300 BOOTP)."""
    assert len(mac) == 6 and len(xid) == 4
    bootp = bytearray(300)
    bootp[0], bootp[1], bootp[2], bootp[3] = 1, 1, 6, 0
    bootp[4:8] = xid
    bootp[10] = 0x80  # broadcast flag
    bootp[28:34] = mac
    bootp[236:240] = bytes([99, 130, 83, 99])
    bootp[240:244] = bytes([53, 1, 1, 255])  # DISCOVER + END
    udp = bytearray(8 + 300)
    udp[0], udp[1], udp[2], udp[3] = 0, 68, 0, 67
    udp[4], udp[5] = ((8 + 300) >> 8) & 0xFF, (8 + 300) & 0xFF
    udp[8:] = bootp
    _udp_cksum(udp, bytes([0, 0, 0, 0]), bytes([255, 255, 255, 255]))
    ip = bytearray(20)
    ip[0], ip[1] = 0x45, 0
    ip[2], ip[3] = ((20 + 8 + 300) >> 8) & 0xFF, (20 + 8 + 300) & 0xFF
    ip[4], ip[5], ip[6], ip[7] = 0x11, 0x22, 0, 0
    ip[8], ip[9] = 64, 17
    ip[12:16] = bytes([0, 0, 0, 0])
    ip[16:20] = bytes([255, 255, 255, 255])
    _ip_cksum(ip)
    eth = bytearray(14 + 20 + 8 + 300)
    eth[0:6] = BROADCAST
    eth[6:12] = mac
    eth[12], eth[13] = 0x08, 0x00
    eth[14:34] = ip
    eth[34:] = udp
    return bytes(eth)


def build_request(mac, xid, req_ip, srv_ip):
    """DHCP REQUEST Ethernet frame (354B: opts grow by 12)."""
    assert len(mac) == 6 and len(xid) == 4
    bootp = bytearray(300)
    bootp[0], bootp[1], bootp[2], bootp[3] = 1, 1, 6, 0
    bootp[4:8] = xid
    bootp[10] = 0x80
    bootp[28:34] = mac
    bootp[236:240] = bytes([99, 130, 83, 99])
    o = 240
    for opt in (bytes([53, 1, 3]),
                bytes([50, 4]) + bytes(req_ip),
                bytes([54, 4]) + bytes(srv_ip),
                bytes([12]) + bytes([8]) + b"pico-eth",
                bytes([255])):
        bootp[o:o + len(opt)] = opt
        o += len(opt)
    udp = bytearray(8 + 300)
    udp[0], udp[1], udp[2], udp[3] = 0, 68, 0, 67
    udp[4], udp[5] = ((8 + 300) >> 8) & 0xFF, (8 + 300) & 0xFF
    udp[8:] = bootp
    _udp_cksum(udp, bytes([0, 0, 0, 0]), bytes([255, 255, 255, 255]))
    ip = bytearray(20)
    ip[0], ip[1] = 0x45, 0
    ip[2], ip[3] = ((20 + 8 + 300) >> 8) & 0xFF, (20 + 8 + 300) & 0xFF
    ip[4], ip[5], ip[6], ip[7] = 0x33, 0x44, 0, 0
    ip[8], ip[9] = 64, 17
    ip[12:16] = bytes([0, 0, 0, 0])
    ip[16:20] = bytes([255, 255, 255, 255])
    _ip_cksum(ip)
    eth = bytearray(14 + 20 + 8 + 300)
    eth[0:6] = BROADCAST
    eth[6:12] = mac
    eth[12], eth[13] = 0x08, 0x00
    eth[14:34] = ip
    eth[34:] = udp
    return bytes(eth)


def _ip_cksum(ip):
    s = 0
    for i in range(0, 20, 2):
        s += (ip[i] << 8) + ip[i + 1]
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    s = (~s) & 0xFFFF
    ip[10], ip[11] = (s >> 8) & 0xFF, s & 0xFF


def _udp_cksum(udp, src, dst):
    ulen = len(udp)
    pseudo = bytes(src) + bytes(dst) + bytes([0, 17]) + bytes(
        [(ulen >> 8) & 0xFF, ulen & 0xFF])
    s = 0
    data = bytes(pseudo) + bytes(udp)
    for i in range(0, len(data) - 1, 2):
        s += (data[i] << 8) + data[i + 1]
    if len(data) & 1:
        s += data[-1] << 8
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    s = (~s) & 0xFFFF
    udp[6], udp[7] = (s >> 8) & 0xFF, s & 0xFF


def parse_offer(frame, xid):
    """Extract (yiaddr, server_id, msgtype) from a DHCP reply frame.
    Returns None if not a matching reply."""
    if len(frame) < 14 + 20 + 8 + 240:
        return None
    if frame[12] != 0x08 or frame[13] != 0x00:
        return None
    ip = frame[14:]
    if ip[9] != 17:
        return None
    bootp = frame[14 + 20 + 8:]
    if bootp[0] != 2 or bootp[236:240] != bytes([99, 130, 83, 99]):
        return None
    if bytes(bootp[4:8]) != bytes(xid):
        return None
    yiaddr = bytes(bootp[16:20])
    msgtype, server = 0, bytes([192, 168, 4, 1])
    i = 240
    while i < len(bootp) and bootp[i] != 255:
        if i + 1 >= len(bootp):
            break
        ln = bootp[i + 1]
        if bootp[i] == 53 and ln >= 1:
            msgtype = bootp[i + 2]
        if bootp[i] == 54 and ln == 4:
            server = bytes(bootp[i + 2:i + 6])
        i += 2 + ln
    return yiaddr, server, msgtype
