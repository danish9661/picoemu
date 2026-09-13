package main

// Userspace NAT64 + DNS64 for guest IPv6 egress.
//
// The gVisor VN behind each room is IPv4-only, so guest IPv6 packets with
// off-link destinations would otherwise die in the pipe. This engine
// translates them to host IPv4 (kernel sockets do the v4 work, so no root
// or raw sockets are needed):
//
//   - TCP/UDP to 64:ff9b::/96 (WELL-KNOWN NAT64 prefix): stateful
//     address+port mapping guest-ULA <-> host socket, full handshake
//     proxying for TCP (SYN/SYN-ACK/ACK, seq/ack translation, FIN/RST).
//   - DNS64: UDP (and proxied pass-through for TCP) port 53 addressed to
//     the gateway ULA fd00:4::1. AAAA answers pass through; A-only names
//     get synthesized AAAA records in 64:ff9b::/96 (RFC 6147, first-order:
//     all A records mapped, TTL clamped).
//   - ICMPv6 echo and anything else (link-local, multicast, on-link
//     fd00:4::/64, gateway addresses) is NOT translated and falls through
//     to the previous path (room broadcast + gVisor, i.e. same as before).
//
// Reverse traffic is unicast to the mapping owner's WS client. Mappings
// are room-scoped and die with the room context. No internet is needed
// for the unit tests: they run against loopback servers addressed via
// 64:ff9b::7f00:1 (127.0.0.1).

import (
	"encoding/binary"
	"fmt"
	"net"
	"sync"
	"time"

	"github.com/gorilla/websocket"
	"github.com/miekg/dns"
)

var (
	// nat64Prefix is 64:ff9b::/96 in 16-byte form.
	nat64Prefix = net.IP([]byte{0x00, 0x64, 0xFF, 0x9B, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})
	nat64PreLen = 12
	dns64TTLMax = uint32(600)
)

const (
	tcpStateDialing = iota
	tcpStateEstab
	tcpStateFinGuest // guest FIN seen, still relaying host->guest
	tcpStateFinHost  // host EOF seen, FIN sent, awaiting guest FIN
)

type nat64TCP struct {
	guestMAC  net.HardwareAddr
	guestIP   net.IP
	guestPort uint16
	dstIP6    net.IP
	dstPort   uint16
	owner     *Client
	conn      *net.TCPConn
	mu        sync.Mutex
	state     int
	peerISN   uint32
	iss       uint32
	rcvNxt    uint32 // next guest seq we expect
	sndNxt    uint32 // next seq we send
	lastUse   time.Time
}

type nat64UDP struct {
	guestMAC  net.HardwareAddr
	guestIP   net.IP
	guestPort uint16
	dstIP6    net.IP
	dstPort   uint16
	owner     *Client
	pc        *net.UDPConn
	lastUse   time.Time
	done      chan struct{}
}

type Nat64Engine struct {
	mu   sync.Mutex
	tcp  map[string]*nat64TCP
	udp  map[string]*nat64UDP
	ctxDone <-chan struct{}
}

func newNat64Engine(ctxDone <-chan struct{}) *Nat64Engine {
	return &Nat64Engine{
		tcp:     make(map[string]*nat64TCP),
		udp:     make(map[string]*nat64UDP),
		ctxDone: ctxDone,
	}
}

func tcpKey(s6 net.IP, sp uint16, d6 net.IP, dp uint16) string {
	return "T" + string(s6.To16()) + string([]byte{byte(sp >> 8), byte(sp)}) +
		string(d6.To16()) + string([]byte{byte(dp >> 8), byte(dp)})
}

func udpKey(s6 net.IP, sp uint16, d6 net.IP, dp uint16) string {
	return "U" + string(s6.To16()) + string([]byte{byte(sp >> 8), byte(sp)}) +
		string(d6.To16()) + string([]byte{byte(dp >> 8), byte(dp)})
}

func inWKP(ip net.IP) bool {
	ip = ip.To16()
	if ip == nil {
		return false
	}
	for i := 0; i < nat64PreLen; i++ {
		if ip[i] != nat64Prefix[i] {
			return false
		}
	}
	return true
}

func wkpToV4(ip net.IP) net.IP {
	ip = ip.To16()
	if ip == nil || !inWKP(ip) {
		return nil
	}
	return net.IPv4(ip[12], ip[13], ip[14], ip[15])
}

func isV6LinkLocal(ip net.IP) bool {
	ip = ip.To16()
	return ip != nil && ip[0] == 0xFE && ip[1]&0xC0 == 0x80
}

func isV6Multicast(ip net.IP) bool {
	ip = ip.To16()
	return ip != nil && ip[0] == 0xFF
}

func isV6OnLink(ip net.IP) bool {
	ip = ip.To16()
	if ip == nil {
		return false
	}
	for i := 0; i < 8; i++ {
		if ip[i] != ip6Pre[i] {
			return false
		}
	}
	return true
}

// ip6UpperSum is the TCP/UDP-over-IPv6 pseudo-header checksum.
func ip6UpperSum(src, dst net.IP, proto byte, payload []byte) uint16 {
	s := ip6PseudoSum(src, dst, proto, len(payload))
	for i := 0; i+1 < len(payload); i += 2 {
		s += uint32(payload[i])<<8 | uint32(payload[i+1])
	}
	if len(payload)&1 != 0 {
		s += uint32(payload[len(payload)-1]) << 8
	}
	for s>>16 != 0 {
		s = (s & 0xFFFF) + (s >> 16)
	}
	return uint16(^s & 0xFFFF)
}

func ip6PseudoSum(src, dst net.IP, proto byte, ln int) uint32 {
	s := uint32(0)
	sb, db := src.To16(), dst.To16()
	for i := 0; i < 16; i += 2 {
		s += uint32(sb[i])<<8 | uint32(sb[i+1])
		s += uint32(db[i])<<8 | uint32(db[i+1])
	}
	s += uint32(ln)
	s += uint32(proto)
	return s
}

// buildIP6Reply lays out a full ETH frame addressed to the guest.
func buildIP6Reply(guestMAC net.HardwareAddr, guestIP, srcIP net.IP, nxt byte, hop byte, payload []byte) []byte {
	f := make([]byte, 14+40+len(payload))
	copy(f[0:], guestMAC)
	copy(f[6:], gwMAC6)
	f[12], f[13] = 0x86, 0xDD
	f[14] = 0x60
	binary.BigEndian.PutUint16(f[18:], uint16(len(payload)))
	f[20], f[21] = nxt, hop
	copy(f[22:], srcIP.To16())
	copy(f[38:], guestIP.To16())
	copy(f[54:], payload)
	return f
}

func sendToOwner(room *Room, owner *Client, frame []byte) {
	room.Lock()
	_, ok := room.Clients[owner]
	room.Unlock()
	if !ok {
		return
	}
	owner.WriteMutex.Lock()
	owner.Conn.WriteMessage(websocket.BinaryMessage, frame)
	owner.WriteMutex.Unlock()
}

// parseV6Transport splits an ETH frame into IP6 addrs + transport header.
// Returns ok=false for non-IPv6, short frames, or extension headers
// (lwIP guests never emit those; fall through to the old path).
func parseV6Transport(msg []byte) (src, dst net.IP, nxt byte, th []byte, ok bool) {
	if len(msg) < 14+40+8 {
		return nil, nil, 0, nil, false
	}
	if binary.BigEndian.Uint16(msg[12:14]) != 0x86DD {
		return nil, nil, 0, nil, false
	}
	ip6 := msg[14:]
	if ip6[0]>>4 != 6 {
		return nil, nil, 0, nil, false
	}
	nxt = ip6[6]
	if nxt != 6 && nxt != 17 {
		return nil, nil, 0, nil, false
	}
	plen := int(binary.BigEndian.Uint16(ip6[4:6]))
	if len(msg) < 14+40+plen {
		return nil, nil, 0, nil, false
	}
	src = net.IP(append([]byte{}, ip6[8:24]...))
	dst = net.IP(append([]byte{}, ip6[24:40]...))
	th = msg[54 : 54+plen]
	return src, dst, nxt, th, true
}

// handleNAT64 translates guest->internet IPv6 TCP/UDP (WKP destinations)
// and gateway-ULA DNS64 queries. Returns true if consumed (caller must
// skip the gVisor uplink); room-local traffic returns false untouched.
func handleNAT64(msg []byte, client *Client, room *Room) bool {
	src6, dst6, nxt, th, ok := parseV6Transport(msg)
	if !ok {
		return false
	}
	if room.NAT64 == nil {
		return false
	}
	guestMAC := net.HardwareAddr(append([]byte{}, msg[6:12]...))

	// DNS64: UDP port 53 to the gateway ULA only (never hijack peers).
	if nxt == 17 && dst6.Equal(gwULA) {
		if len(th) >= 8 && binary.BigEndian.Uint16(th[2:4]) == 53 {
			return room.NAT64.handleDNS64(msg, guestMAC, src6, th, client, room)
		}
		return false
	}

	// Room-local destinations keep the old path.
	if isV6Multicast(dst6) || isV6LinkLocal(dst6) || isV6OnLink(dst6) ||
		dst6.Equal(gwLL) || dst6.Equal(gwULA) {
		return false
	}
	if !inWKP(dst6) {
		return false
	}
	dst4 := wkpToV4(dst6)
	if dst4 == nil {
		return false
	}
	switch nxt {
	case 17:
		return room.NAT64.handleUDP(msg, guestMAC, src6, dst6, dst4, th, client, room)
	case 6:
		return room.NAT64.handleTCP(msg, guestMAC, src6, dst6, dst4, th, client, room)
	}
	return false
}

// ---- UDP ----

func (e *Nat64Engine) handleUDP(msg []byte, guestMAC net.HardwareAddr, src6, dst6, dst4 net.IP, th []byte, client *Client, room *Room) bool {
	if len(th) < 8 {
		return true
	}
	sport := binary.BigEndian.Uint16(th[0:2])
	dport := binary.BigEndian.Uint16(th[2:4])
	ulen := int(binary.BigEndian.Uint16(th[4:6]))
	if ulen < 8 || ulen > len(th) {
		return true
	}
	payload := append([]byte{}, th[8:ulen]...)
	key := udpKey(src6, sport, dst6, dport)

	e.mu.Lock()
	m, ok := e.udp[key]
	if !ok {
		raddr := &net.UDPAddr{IP: dst4, Port: int(dport)}
		pc, err := net.DialUDP("udp4", nil, raddr)
		if err != nil {
			e.mu.Unlock()
			fmt.Printf("[NAT64] UDP dial %v:%d: %v\n", dst4, dport, err)
			return true
		}
		m = &nat64UDP{
			guestMAC: append(net.HardwareAddr{}, guestMAC...), guestIP: append(net.IP{}, src6...),
			guestPort: sport, dstIP6: append(net.IP{}, dst6...), dstPort: dport,
			owner: client, pc: pc, lastUse: time.Now(), done: make(chan struct{}),
		}
		e.udp[key] = m
		e.mu.Unlock()
		go e.udpUpstreamLoop(room, key, m)
	} else {
		m.lastUse = time.Now()
		m.owner = client
		e.mu.Unlock()
	}
	if _, err := m.pc.Write(payload); err != nil {
		fmt.Printf("[NAT64] UDP write: %v\n", err)
	}
	return true
}

func (e *Nat64Engine) udpUpstreamLoop(room *Room, key string, m *nat64UDP) {
	defer m.pc.Close()
	buf := make([]byte, 2048)
	for {
		select {
		case <-e.ctxDone:
			return
		case <-m.done:
			return
		default:
		}
		m.pc.SetReadDeadline(time.Now().Add(2 * time.Second))
		n, err := m.pc.Read(buf)
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				continue
			}
			e.mu.Lock()
			delete(e.udp, key)
			e.mu.Unlock()
			return
		}
		uh := make([]byte, 8+n)
		binary.BigEndian.PutUint16(uh[0:2], m.dstPort)
		binary.BigEndian.PutUint16(uh[2:4], m.guestPort)
		binary.BigEndian.PutUint16(uh[4:6], uint16(8+n))
		binary.BigEndian.PutUint16(uh[6:8], 0)
		copy(uh[8:], buf[:n])
		cs := ip6UpperSum(m.dstIP6, m.guestIP, 17, uh)
		if cs == 0 {
			cs = 0xFFFF
		}
		binary.BigEndian.PutUint16(uh[6:8], cs)
		frame := buildIP6Reply(m.guestMAC, m.guestIP, m.dstIP6, 17, 64, uh)
		e.mu.Lock()
		m.lastUse = time.Now()
		e.mu.Unlock()
		sendToOwner(room, m.owner, frame)
	}
}

// ---- TCP ----

func tcpFlags(b byte) (fin, syn, rst, ack bool) {
	return b&0x01 != 0, b&0x02 != 0, b&0x04 != 0, b&0x10 != 0
}

func (e *Nat64Engine) sendTCPSeg(room *Room, owner *Client, guestMAC net.HardwareAddr, src6, dst6 net.IP, sport, dport uint16, seq, ack uint32, flags byte, payload []byte, mss bool) {
	th := make([]byte, 20+len(payload))
	if mss {
		th = make([]byte, 24+len(payload))
		th[20], th[21], th[22], th[23] = 2, 4, 0x05, 0xA0 // MSS 1440
	}
	binary.BigEndian.PutUint16(th[0:2], sport)
	binary.BigEndian.PutUint16(th[2:4], dport)
	binary.BigEndian.PutUint32(th[4:8], seq)
	binary.BigEndian.PutUint32(th[8:12], ack)
	th[12] = byte((len(th) - len(payload)) / 4 << 4)
	th[13] = flags
	binary.BigEndian.PutUint16(th[14:16], 65535)
	binary.BigEndian.PutUint16(th[16:18], 0)
	copy(th[len(th)-len(payload):], payload)
	cs := ip6UpperSum(src6, dst6, 6, th)
	if cs == 0 {
		cs = 0xFFFF
	}
	binary.BigEndian.PutUint16(th[16:18], cs)
	frame := buildIP6Reply(guestMAC, dst6, src6, 6, 64, th)
	sendToOwner(room, owner, frame)
}

func (e *Nat64Engine) sendRST(room *Room, owner *Client, guestMAC net.HardwareAddr, src6, dst6 net.IP, sport, dport uint16, seq uint32) {
	e.sendTCPSeg(room, owner, guestMAC, dst6, src6, dport, sport, 0, seq, 0x14, nil, false)
}

func (e *Nat64Engine) handleTCP(msg []byte, guestMAC net.HardwareAddr, src6, dst6, dst4 net.IP, th []byte, client *Client, room *Room) bool {
	if len(th) < 20 {
		return true
	}
	sport := binary.BigEndian.Uint16(th[0:2])
	dport := binary.BigEndian.Uint16(th[2:4])
	seq := binary.BigEndian.Uint32(th[4:8])
	// ack := binary.BigEndian.Uint32(th[8:12])
	doff := int(th[12]>>4) * 4
	if doff < 20 || doff > len(th) {
		return true
	}
	fin, syn, rst, _ := tcpFlags(th[13])
	payload := append([]byte{}, th[doff:]...)
	key := tcpKey(src6, sport, dst6, dport)

	e.mu.Lock()
	m, ok := e.tcp[key]
	if !ok {
		if !syn || rst {
			e.mu.Unlock()
			return true // stray non-SYN for unknown flow: drop (guest retransmits)
		}
		m = &nat64TCP{
			guestMAC: append(net.HardwareAddr{}, guestMAC...), guestIP: append(net.IP{}, src6...),
			guestPort: sport, dstIP6: append(net.IP{}, dst6...), dstPort: dport,
			owner: client, state: tcpStateDialing, peerISN: seq,
			rcvNxt: seq + 1, lastUse: time.Now(),
		}
		e.tcp[key] = m
		e.mu.Unlock()
		go e.tcpDial(room, key, m, dst4)
		return true
	}
	m.owner = client
	m.lastUse = time.Now()
	state := m.state
	e.mu.Unlock()

	m.mu.Lock()
	defer m.mu.Unlock()
	switch {
	case rst:
		m.conn.Close()
		e.dropTCP(key)
		return true
	case syn && state == tcpStateEstab:
		// Duplicate SYN (our SYN-ACK was lost): resend it verbatim.
		e.sendTCPSeg(room, m.owner, m.guestMAC, m.dstIP6, m.guestIP, m.dstPort, m.guestPort, m.iss, m.peerISN+1, 0x12, nil, true)
		return true
	case state != tcpStateEstab && state != tcpStateFinHost:
		// Dialing (SYN retransmits: dial covers the original ISN, the
		// SYN-ACK will match) or guest-FINed (nothing left to consume):
		// drop; retransmit covers.
		return true
	}
	// Established (or host-FINed): accept in-order bytes only.
	if seq != m.rcvNxt {
		if len(payload) == 0 && !fin {
			return true // dup ACK, nothing to do
		}
		// Out-of-order: re-ACK current position so the guest retransmits.
		e.sendTCPSeg(room, m.owner, m.guestMAC, m.dstIP6, m.guestIP, m.dstPort, m.guestPort, m.sndNxt, m.rcvNxt, 0x10, nil, false)
		return true
	}
	if len(payload) > 0 {
		if state == tcpStateFinHost {
			e.sendRST(room, m.owner, m.guestMAC, m.guestIP, m.dstIP6, m.guestPort, m.dstPort, m.rcvNxt)
			return true
		}
		if _, err := m.conn.Write(payload); err != nil {
			e.sendRST(room, m.owner, m.guestMAC, m.guestIP, m.dstIP6, m.guestPort, m.dstPort, m.rcvNxt)
			m.conn.Close()
			e.dropTCP(key)
			return true
		}
		m.rcvNxt += uint32(len(payload))
	}
	if fin {
		m.rcvNxt++
		m.conn.CloseWrite()
		if m.state == tcpStateFinHost {
			m.conn.Close()
			e.dropTCP(key)
			return true
		}
		m.state = tcpStateFinGuest
	}
	// ACK everything consumed so far.
	e.sendTCPSeg(room, m.owner, m.guestMAC, m.dstIP6, m.guestIP, m.dstPort, m.guestPort, m.sndNxt, m.rcvNxt, 0x10, nil, false)
	return true
}

func (e *Nat64Engine) dropTCP(key string) {
	if m, ok := e.tcp[key]; ok {
		_ = m
		delete(e.tcp, key)
	}
}

func (e *Nat64Engine) tcpDial(room *Room, key string, m *nat64TCP, dst4 net.IP) {
	d := net.Dialer{Timeout: 10 * time.Second}
	c, err := d.Dial("tcp4", net.JoinHostPort(dst4.String(), fmt.Sprintf("%d", m.dstPort)))
	var conn *net.TCPConn
	if err == nil {
		var ok bool
		if conn, ok = c.(*net.TCPConn); !ok {
			c.Close()
			err = fmt.Errorf("not TCP")
		} else {
			_ = conn.SetNoDelay(true)
		}
	}
	e.mu.Lock()
	defer e.mu.Unlock()
	cur, ok := e.tcp[key]
	if !ok || cur != m {
		if err == nil {
			conn.Close()
		}
		return // superseded / gone
	}
	if err != nil {
		fmt.Printf("[NAT64] TCP dial %v:%d: %v\n", dst4, m.dstPort, err)
		m.mu.Lock()
		isn := m.peerISN
		m.mu.Unlock()
		e.sendRST(room, m.owner, m.guestMAC, m.guestIP, m.dstIP6, m.guestPort, m.dstPort, isn+1)
		delete(e.tcp, key)
		return
	}
	m.mu.Lock()
	m.conn = conn
	// Simple time-based ISS; enough for a test gateway.
	m.iss = uint32(time.Now().UnixNano() & 0xFFFFFFFF)
	m.sndNxt = m.iss + 1
	m.state = tcpStateEstab
	m.lastUse = time.Now()
	iss, ack := m.iss, m.peerISN+1
	m.mu.Unlock()
	e.sendTCPSeg(room, m.owner, m.guestMAC, m.dstIP6, m.guestIP, m.dstPort, m.guestPort, iss, ack, 0x12, nil, true)
	go e.tcpUpstreamLoop(room, key, m)
}

func (e *Nat64Engine) tcpUpstreamLoop(room *Room, key string, m *nat64TCP) {
	defer m.conn.Close()
	buf := make([]byte, 1460)
	for {
		select {
		case <-e.ctxDone:
			return
		default:
		}
		m.conn.SetReadDeadline(time.Now().Add(2 * time.Second))
		n, err := m.conn.Read(buf)
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				continue
			}
			// Host EOF/error: FIN the guest (once), then drop.
			m.mu.Lock()
			if m.state == tcpStateEstab || m.state == tcpStateFinGuest {
				e.sendTCPSeg(room, m.owner, m.guestMAC, m.dstIP6, m.guestIP, m.dstPort, m.guestPort, m.sndNxt, m.rcvNxt, 0x11, nil, false)
				m.sndNxt++
				if m.state == tcpStateFinGuest {
					m.mu.Unlock()
					e.mu.Lock()
					e.dropTCP(key)
					e.mu.Unlock()
					return
				}
				m.state = tcpStateFinHost
			}
			m.mu.Unlock()
			if m.state == tcpStateFinHost {
				// Wait a grace period for the guest FIN, then give up.
				select {
				case <-e.ctxDone:
				case <-time.After(30 * time.Second):
				}
			}
			e.mu.Lock()
			e.dropTCP(key)
			e.mu.Unlock()
			return
		}
		m.mu.Lock()
		pl := append([]byte{}, buf[:n]...)
		e.sendTCPSeg(room, m.owner, m.guestMAC, m.dstIP6, m.guestIP, m.dstPort, m.guestPort, m.sndNxt, m.rcvNxt, 0x18, pl, false)
		m.sndNxt += uint32(n)
		m.lastUse = time.Now()
		m.mu.Unlock()
	}
}

// ---- sweeper ----

func (e *Nat64Engine) sweepLoop() {
	t := time.NewTicker(10 * time.Second)
	defer t.Stop()
	for {
		select {
		case <-e.ctxDone:
			return
		case <-t.C:
			now := time.Now()
			e.mu.Lock()
			for k, m := range e.udp {
				if now.Sub(m.lastUse) > 90*time.Second {
					close(m.done)
					m.pc.Close()
					delete(e.udp, k)
				}
			}
			for k, m := range e.tcp {
				m.mu.Lock()
				idle := now.Sub(m.lastUse)
				m.mu.Unlock()
				if idle > 600*time.Second {
					m.conn.Close()
					delete(e.tcp, k)
				}
			}
			e.mu.Unlock()
		}
	}
}

// ---- DNS64 ----

var dnsUpstreamOnce = struct {
	sync.Once
	servers []string
}{}

func dnsUpstream() []string {
	dnsUpstreamOnce.Do(func() {
		if cfg, err := dns.ClientConfigFromFile("/etc/resolv.conf"); err == nil && len(cfg.Servers) > 0 {
			dnsUpstreamOnce.servers = append([]string{}, cfg.Servers...)
		} else {
			dnsUpstreamOnce.servers = []string{"8.8.8.8:53", "1.1.1.1:53"}
		}
	})
	return dnsUpstreamOnce.servers
}

func dnsExchange(query []byte, qtype uint16) (*dns.Msg, error) {
	req := new(dns.Msg)
	if err := req.Unpack(query); err != nil {
		return nil, err
	}
	servers := dnsUpstream()
	if testDNSServer != "" {
		servers = []string{testDNSServer}
	}
	c := new(dns.Client)
	c.Timeout = 4 * time.Second
	var last error
	for _, s := range servers {
		req.Id = dns.Id()
		req.Question[0].Qtype = qtype
		resp, _, err := c.Exchange(req, s)
		if err == nil && resp != nil {
			return resp, nil
		}
		last = err
	}
	return nil, last
}

// testDNSServer overrides upstream resolution in unit tests.
var testDNSServer = ""

func (e *Nat64Engine) handleDNS64(msg []byte, guestMAC net.HardwareAddr, src6 net.IP, th []byte, client *Client, room *Room) bool {
	if len(th) < 8 {
		return true
	}
	sport := binary.BigEndian.Uint16(th[0:2])
	query := append([]byte{}, th[8:]...)
	req := new(dns.Msg)
	if req.Unpack(query) != nil || len(req.Question) == 0 {
		return true
	}
	q := req.Question[0]
	var out *dns.Msg
	switch q.Qtype {
	case dns.TypeAAAA:
		resp, err := dnsExchange(query, dns.TypeAAAA)
		if err == nil && resp != nil && resp.Rcode == dns.RcodeSuccess && hasRRType(resp, dns.TypeAAAA) {
			out = resp
			break
		}
		arec, err := dnsExchange(query, dns.TypeA)
		if err != nil || arec == nil {
			return true
		}
		if arec.Rcode != dns.RcodeSuccess || !hasRRType(arec, dns.TypeA) {
			out = arec // NODATA/NXDOMAIN passthrough
			break
		}
		out = new(dns.Msg)
		out.SetReply(req)
		for _, rr := range arec.Answer {
			if a, ok := rr.(*dns.A); ok {
				ttl := a.Hdr.Ttl
				if ttl > dns64TTLMax {
					ttl = dns64TTLMax
				}
			out.Answer = append(out.Answer, &dns.AAAA{
				Hdr: dns.RR_Header{Name: a.Hdr.Name, Rrtype: dns.TypeAAAA, Class: a.Hdr.Class, Ttl: ttl},
				AAAA: append(append(net.IP{}, nat64Prefix[:12]...), 0, 0, 0, 0),
			})
			copy(out.Answer[len(out.Answer)-1].(*dns.AAAA).AAAA[12:], a.A.To4())
			}
		}
	case dns.TypeA:
		resp, err := dnsExchange(query, dns.TypeA)
		if err != nil || resp == nil {
			return true
		}
		out = resp
	default:
		resp, err := dnsExchange(query, q.Qtype)
		if err != nil || resp == nil {
			return true
		}
		out = resp
	}
	if out == nil {
		return true
	}
	packed, err := out.Pack()
	if err != nil {
		return true
	}
	uh := make([]byte, 8+len(packed))
	binary.BigEndian.PutUint16(uh[0:2], 53)
	binary.BigEndian.PutUint16(uh[2:4], sport)
	binary.BigEndian.PutUint16(uh[4:6], uint16(8+len(packed)))
	binary.BigEndian.PutUint16(uh[6:8], 0)
	copy(uh[8:], packed)
	cs := ip6UpperSum(gwULA, src6, 17, uh)
	if cs == 0 {
		cs = 0xFFFF
	}
	binary.BigEndian.PutUint16(uh[6:8], cs)
	frame := buildIP6Reply(guestMAC, src6, gwULA, 17, 64, uh)
	sendToOwner(room, client, frame)
	fmt.Printf("[DNS64] %s %s -> %d answer(s)\n", q.Name, dns.TypeToString[q.Qtype], len(out.Answer))
	return true
}

func hasRRType(m *dns.Msg, t uint16) bool {
	for _, rr := range m.Answer {
		if rr.Header().Rrtype == t {
			return true
		}
	}
	return false
}
