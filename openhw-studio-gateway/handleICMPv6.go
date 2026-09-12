package main

import (
	"encoding/binary"
	"fmt"
	"net"
	"time"

	"github.com/gorilla/websocket"
)

// IPv6 gateway identity (mirrors the emulator fake network so guests see
// one consistent router whether fake services or the gateway answer).
var (
	gwMAC6 = net.HardwareAddr{0x5a, 0x94, 0xef, 0xe4, 0x0c, 0xdd}
	gwLL   = net.ParseIP("fe80::1").To16()
	gwULA  = net.ParseIP("fd00:4::1").To16()
	ip6Pre = net.IP([]byte{0xFD, 0x00, 0x00, 0x04, 0, 0, 0, 0})
	ip6All = net.ParseIP("ff02::1").To16()
	ip6Uns = net.IP(make([]byte, 16))
	mcAll  = net.HardwareAddr{0x33, 0x33, 0, 0, 0, 1}
)

func icmp6Sum(src, dst net.IP, pl []byte) uint16 {
	sum := uint32(0)
	for i := 0; i < 16; i += 2 {
		sum += uint32(src[i])<<8 | uint32(src[i+1])
		sum += uint32(dst[i])<<8 | uint32(dst[i+1])
	}
	sum += uint32(len(pl))
	sum += 58
	for i := 0; i+1 < len(pl); i += 2 {
		sum += uint32(pl[i])<<8 | uint32(pl[i+1])
	}
	if len(pl)&1 != 0 {
		sum += uint32(pl[len(pl)-1]) << 8
	}
	for sum>>16 != 0 {
		sum = (sum & 0xFFFF) + (sum >> 16)
	}
	return uint16(^sum & 0xFFFF)
}

// icmp6Start lays out ETH+IPv6 headers, returns offset of the ICMPv6 body.
func icmp6Start(buf []byte, dstMAC net.HardwareAddr, srcIP, dstIP net.IP, icmpLen int) int {
	off := 0
	copy(buf[off:], dstMAC)
	off += 6
	copy(buf[off:], gwMAC6)
	off += 6
	buf[off], buf[off+1] = 0x86, 0xDD
	off += 2
	buf[off], buf[off+1], buf[off+2], buf[off+3] = 0x60, 0, 0, 0
	off += 4
	binary.BigEndian.PutUint16(buf[off:], uint16(icmpLen))
	off += 2
	buf[off], buf[off+1] = 58, 255 // ICMPv6, hop limit 255 (NDP)
	off += 2
	copy(buf[off:], srcIP.To16())
	off += 16
	copy(buf[off:], dstIP.To16())
	off += 16
	return off // == 54
}

func icmp6Send(client *Client, frame []byte) {
	client.WriteMutex.Lock()
	client.Conn.WriteMessage(websocket.BinaryMessage, frame)
	client.WriteMutex.Unlock()
}

// buildRA lays out a Router Advertisement to dstIP/dstMAC with the
// fd00:4::/64 SLAAC prefix (same bytes as the RS-triggered reply).
func buildRA(dstIP net.IP, dstMAC net.HardwareAddr) []byte {
	frame := make([]byte, 256)
	off := icmp6Start(frame, dstMAC, gwLL, dstIP, 64)
	io := off
	frame[off], frame[off+1] = 134, 0 // RA
	off += 2
	off += 2 // checksum later
	frame[off] = 64 // cur hop limit
	off++
	frame[off] = 0 // M/O flags
	off++
	binary.BigEndian.PutUint16(frame[off:], 1800) // router lifetime
	off += 2
	off += 8 // reachable + retrans
	frame[off], frame[off+1] = 1, 1 // source link-layer
	off += 2
	copy(frame[off:], gwMAC6)
	off += 6
	frame[off], frame[off+1] = 3, 4 // prefix info
	frame[off+2] = 64
	frame[off+3] = 0xC0 // L + A (SLAAC)
	off += 4
	binary.BigEndian.PutUint32(frame[off:], 86400) // valid
	off += 4
	binary.BigEndian.PutUint32(frame[off:], 14400) // preferred
	off += 4
	off += 4 // reserved
	copy(frame[off:], ip6Pre)
	off += 8
	for i := 0; i < 8; i++ {
		frame[off] = 0
		off++
	}
	frame[off], frame[off+1] = 5, 1 // MTU
	frame[off+2], frame[off+3] = 0, 0
	off += 4
	binary.BigEndian.PutUint32(frame[off:], 1500)
	off += 4
	cs := icmp6Sum(gwLL, dstIP, frame[io:off])
	binary.BigEndian.PutUint16(frame[io+2:], cs)
	return frame[:off]
}

// raTickerLoop multicasts an unsolicited RA (all-nodes) to room members
// every few seconds so timer-less stacks (no RS) still learn the prefix
// via SLAAC. Stops with the room context.
func raTickerLoop(room *Room) {
	t := time.NewTicker(7 * time.Second)
	defer t.Stop()
	for {
		select {
		case <-room.Ctx.Done():
			return
		case <-t.C:
			frame := buildRA(ip6All, mcAll)
			room.Lock()
			targets := make([]*Client, 0, len(room.Clients))
			for client := range room.Clients {
				targets = append(targets, client)
			}
			room.Unlock()
			for _, client := range targets {
				client.WriteMutex.Lock()
				err := client.Conn.WriteMessage(websocket.BinaryMessage, frame)
				client.WriteMutex.Unlock()
				if err != nil {
					fmt.Printf("[Room %s] RA write error: %v\n", room.SessionId, err)
				}
			}
		}
	}
}

// handleICMPv6 answers Router Solicitations (RA with the fd00:4::/64 SLAAC
// prefix), Neighbor Solicitations for gateway addresses (NA), and Echo
// Requests to gateway addresses. Returns true if handled (caller skips
// the gVisor uplink); anything else falls through to room broadcast.
func handleICMPv6(msg []byte, client *Client) bool {
	if len(msg) < 14+40+8 {
		return false
	}
	if binary.BigEndian.Uint16(msg[12:14]) != 0x86DD {
		return false
	}
	ip6 := msg[14:]
	if ip6[0]>>4 != 6 || ip6[6] != 58 {
		return false
	}
	ic := ip6[40:]
	icmpLen := len(msg) - 14 - 40
	srcMAC := net.HardwareAddr(msg[6:12])
	srcIP := net.IP(ip6[8:24])

	frame := make([]byte, 256)

	switch ic[0] {
	case 133: // Router Solicitation -> Advertisement
		dstIP := srcIP
		dstMAC := srcMAC
		if srcIP.Equal(ip6Uns) {
			dstIP = ip6All
			dstMAC = mcAll
		}
		frame := buildRA(dstIP, dstMAC)
		fmt.Printf("[ICMPv6] RS -> RA (%dB)\n", len(frame))
		icmp6Send(client, frame)
		return true

	case 135: // Neighbor Solicitation for a gateway address -> NA
		if icmpLen < 28 {
			return false
		}
		tgt := net.IP(ic[8:24])
		var gw net.IP
		if tgt.Equal(gwLL) {
			gw = gwLL
		} else if tgt.Equal(gwULA) {
			gw = gwULA
		} else {
			return false
		}
		off := icmp6Start(frame, srcMAC, gw, srcIP, 32)
		io := off
		frame[off], frame[off+1] = 136, 0 // NA
		off += 2
		off += 2 // checksum later
		frame[off] = 0x60 // solicited + override
		off++
		off += 3 // reserved
		copy(frame[off:], tgt)
		off += 16
		frame[off], frame[off+1] = 2, 1 // target link-layer
		off += 2
		copy(frame[off:], gwMAC6)
		off += 6
		cs := icmp6Sum(gw, srcIP, frame[io:off])
		binary.BigEndian.PutUint16(frame[io+2:], cs)
		fmt.Printf("[ICMPv6] NS(%s) -> NA\n", tgt)
		icmp6Send(client, frame[:off])
		return true

	case 128: // Echo Request to a gateway address -> Echo Reply
		dst := net.IP(ip6[24:40])
		if !dst.Equal(gwLL) && !dst.Equal(gwULA) {
			return false
		}
		bodyLen := icmpLen
		off := icmp6Start(frame, srcMAC, dst, srcIP, bodyLen)
		io := off
		frame[off] = 129 // Echo Reply
		off++
		frame[off] = 0 // code
		off++
		off += 2 // checksum later
		copy(frame[off:], ic[4:icmpLen])
		off += icmpLen - 4
		cs := icmp6Sum(dst, srcIP, frame[io:off])
		binary.BigEndian.PutUint16(frame[io+2:], cs)
		fmt.Printf("[ICMPv6] Echo -> Reply (%dB)\n", off)
		icmp6Send(client, frame[:off])
		return true
	}
	return false
}
