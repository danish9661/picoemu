package main

import (
	"context"
	"encoding/binary"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/miekg/dns"
)

var (
	n6MAC  = net.HardwareAddr{0x02, 0x12, 0x34, 0x56, 0x78, 0x02}
	n6ULA  = net.ParseIP("fd00:4::dcad:beff:feef:cafe").To16()
	n6WKP  = net.ParseIP("64:ff9b::7f00:1").To16()
	n6GUA6 = net.ParseIP("fd00:4::9").To16()
)

func mkIP6(t *testing.T, dstMAC net.HardwareAddr, srcIP, dstIP net.IP, nxt byte, payload []byte) []byte {
	t.Helper()
	f := make([]byte, 14+40+len(payload))
	copy(f[0:], dstMAC)
	copy(f[6:], n6MAC)
	f[12], f[13] = 0x86, 0xDD
	f[14] = 0x60
	binary.BigEndian.PutUint16(f[18:], uint16(len(payload)))
	f[20], f[21] = nxt, 64
	copy(f[22:], srcIP.To16())
	copy(f[38:], dstIP.To16())
	copy(f[54:], payload)
	return f
}

func mkTCPseg(t *testing.T, srcIP, dstIP net.IP, sport, dport uint16, seq, ack uint32, flags byte, payload []byte) []byte {
	t.Helper()
	th := make([]byte, 20+len(payload))
	binary.BigEndian.PutUint16(th[0:2], sport)
	binary.BigEndian.PutUint16(th[2:4], dport)
	binary.BigEndian.PutUint32(th[4:8], seq)
	binary.BigEndian.PutUint32(th[8:12], ack)
	th[12], th[13] = 0x50, flags
	binary.BigEndian.PutUint16(th[14:16], 8192)
	copy(th[20:], payload)
	cs := ip6UpperSum(srcIP, dstIP, 6, th)
	if cs == 0 {
		cs = 0xFFFF
	}
	binary.BigEndian.PutUint16(th[16:18], cs)
	return mkIP6(t, gwMAC6, srcIP, dstIP, 6, th)
}

func mkUDPseg(t *testing.T, srcIP, dstIP net.IP, sport, dport uint16, payload []byte) []byte {
	t.Helper()
	uh := make([]byte, 8+len(payload))
	binary.BigEndian.PutUint16(uh[0:2], sport)
	binary.BigEndian.PutUint16(uh[2:4], dport)
	binary.BigEndian.PutUint16(uh[4:6], uint16(8+len(payload)))
	copy(uh[8:], payload)
	cs := ip6UpperSum(srcIP, dstIP, 17, uh)
	if cs == 0 {
		cs = 0xFFFF
	}
	binary.BigEndian.PutUint16(uh[6:8], cs)
	return mkIP6(t, gwMAC6, srcIP, dstIP, 17, uh)
}

type tcpSeg struct {
	flags   byte
	seq     uint32
	ack     uint32
	payload []byte
	src     net.IP
	dst     net.IP
}

func readSeg(t *testing.T, c *websocket.Conn, timeout time.Duration) *tcpSeg {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		f := wsRead(t, c, time.Until(deadline))
		if f == nil {
			t.Fatal("no frame")
		}
		if len(f) < 14+40+20 || f[12] != 0x86 || f[13] != 0xDD {
			continue
		}
		ip := f[14:]
		if ip[0]>>4 != 6 || ip[6] != 6 {
			continue
		}
		ln := int(binary.BigEndian.Uint16(ip[4:6]))
		th := ip[40 : 40+ln]
		if ip6UpperSum(ip[8:24], ip[24:40], 6, th) != 0 {
			t.Fatal("bad TCP checksum")
		}
		doff := int(th[12]>>4) * 4
		return &tcpSeg{flags: th[13], seq: binary.BigEndian.Uint32(th[4:8]),
			ack: binary.BigEndian.Uint32(th[8:12]), payload: append([]byte{}, th[doff:]...),
			src: net.IP(append([]byte{}, ip[8:24]...)), dst: net.IP(append([]byte{}, ip[24:40]...))}
	}
	t.Fatal("timeout")
	return nil
}

// readData skips pure ACKs and returns the next payload bytes.
func readData(t *testing.T, c *websocket.Conn, timeout time.Duration) []byte {
	t.Helper()
	deadline := time.Now().Add(timeout)
	var out []byte
	for time.Now().Before(deadline) {
		s := readSeg(t, c, time.Until(deadline))
		if len(s.payload) > 0 {
			out = append(out, s.payload...)
			return out
		}
	}
	t.Fatal("no data")
	return nil
}

func withNAT64Server(t *testing.T) (string, *Room, func()) {
	t.Helper()
	ctx, cancel := context.WithCancel(context.Background())
	room := &Room{Clients: make(map[*Client]bool), Ctx: ctx, Cancel: cancel, MacToIP: make(map[string]net.IP)}
	room.NAT64 = newNat64Engine(ctx.Done())
	go room.NAT64.sweepLoop()
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		c, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer c.Close()
		cl := &Client{Conn: c}
		room.Lock()
		room.Clients[cl] = true
		room.Unlock()
		defer func() {
			room.Lock()
			delete(room.Clients, cl)
			room.Unlock()
		}()
		for {
			mt, msg, err := c.ReadMessage()
			if err != nil {
				return
			}
			if mt == websocket.BinaryMessage {
				handleNAT64(msg, cl, room)
			}
		}
	}))
	return "ws" + strings.TrimPrefix(srv.URL, "http"), room, func() { cancel(); srv.Close() }
}

// --- echo servers ---

func startTCPEcho(t *testing.T) (string, func()) {
	t.Helper()
	ln, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			go func(c net.Conn) {
				defer c.Close()
				buf := make([]byte, 2048)
				for {
					n, err := c.Read(buf)
					if err != nil {
						return
					}
					if _, err := c.Write(buf[:n]); err != nil {
						return
					}
				}
			}(c)
		}
	}()
	return ln.Addr().String(), func() { ln.Close() }
}

func startUDPEcho(t *testing.T) (*net.UDPAddr, func()) {
	t.Helper()
	pc, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.ParseIP("127.0.0.1")})
	if err != nil {
		t.Fatal(err)
	}
	go func() {
		buf := make([]byte, 2048)
		for {
			n, addr, err := pc.ReadFromUDP(buf)
			if err != nil {
				return
			}
			pc.WriteToUDP(buf[:n], addr)
		}
	}()
	return pc.LocalAddr().(*net.UDPAddr), func() { pc.Close() }
}

func TestNAT64TCP(t *testing.T) {
	url, _, cleanup := withNAT64Server(t)
	defer cleanup()
	echoAddr, stopEcho := startTCPEcho(t)
	defer stopEcho()
	ta, err := net.ResolveTCPAddr("tcp4", echoAddr)
	if err != nil {
		t.Fatal(err)
	}
	dport := uint16(ta.Port)
	c := wsDial(t, url)
	defer c.Close()

	const isn = 0x11223344
	syn := mkTCPseg(t, n6ULA, n6WKP, 40001, dport, isn, 0, 0x02, nil)
	if err := c.WriteMessage(websocket.BinaryMessage, syn); err != nil {
		t.Fatal(err)
	}
	// Expect SYN-ACK (skip nothing: engine sends no pure noise here).
	var sa *tcpSeg
	deadline := time.Now().Add(10 * time.Second)
	for {
		sa = readSeg(t, c, time.Until(deadline))
		if sa.flags&0x12 == 0x12 {
			break
		}
	}
	if sa.ack != isn+1 {
		t.Fatalf("bad SYN-ACK ack=%x want %x", sa.ack, isn+1)
	}
	if !sa.src.Equal(n6WKP) || !sa.dst.Equal(n6ULA) {
		t.Fatalf("bad SYN-ACK addrs %v -> %v", sa.src, sa.dst)
	}
	// Complete handshake + send data.
	ack := mkTCPseg(t, n6ULA, n6WKP, 40001, dport, isn+1, sa.seq+1, 0x10, nil)
	if err := c.WriteMessage(websocket.BinaryMessage, ack); err != nil {
		t.Fatal(err)
	}
	data := mkTCPseg(t, n6ULA, n6WKP, 40001, dport, isn+1, sa.seq+1, 0x18, []byte("hello-nat64"))
	if err := c.WriteMessage(websocket.BinaryMessage, data); err != nil {
		t.Fatal(err)
	}
	got := readData(t, c, 10*time.Second)
	if string(got) != "hello-nat64" {
		t.Fatalf("bad echo %q", got)
	}
	// Clean close: FIN -> expect FIN back.
	fin := mkTCPseg(t, n6ULA, n6WKP, 40001, dport, isn+1+uint32(len("hello-nat64")), sa.seq+1+uint32(len(got)), 0x11, nil)
	if err := c.WriteMessage(websocket.BinaryMessage, fin); err != nil {
		t.Fatal(err)
	}
	deadline = time.Now().Add(10 * time.Second)
	for {
		s := readSeg(t, c, time.Until(deadline))
		if s.flags&0x01 != 0 {
			return // FIN seen: close dance complete
		}
	}
}

func TestNAT64TCPRefused(t *testing.T) {
	url, _, cleanup := withNAT64Server(t)
	defer cleanup()
	c := wsDial(t, url)
	defer c.Close()
	// Nothing listens on TEST-NET-1 discard-ish high port via loopback-mapped closed port.
	ln, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	closedPort := uint16(ln.Addr().(*net.TCPAddr).Port)
	ln.Close()

	const isn = 0xAABBCCDD
	syn := mkTCPseg(t, n6ULA, n6WKP, 40002, closedPort, isn, 0, 0x02, nil)
	if err := c.WriteMessage(websocket.BinaryMessage, syn); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(15 * time.Second)
	for {
		s := readSeg(t, c, time.Until(deadline))
		if s.flags&0x04 != 0 {
			if s.ack != isn+1 {
				t.Fatalf("bad RST ack=%x", s.ack)
			}
			return
		}
	}
}

func TestNAT64UDP(t *testing.T) {
	url, _, cleanup := withNAT64Server(t)
	defer cleanup()
	uaddr, stopEcho := startUDPEcho(t)
	defer stopEcho()

	c := wsDial(t, url)
	defer c.Close()

	dgram := mkUDPseg(t, n6ULA, n6WKP, 50001, uint16(uaddr.Port), []byte("ping-nat64"))
	if err := c.WriteMessage(websocket.BinaryMessage, dgram); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(10 * time.Second)
	for {
		if time.Now().After(deadline) {
			t.Fatal("no UDP reply")
		}
		c.SetReadDeadline(time.Now().Add(time.Until(deadline)))
		mt, f, err := c.ReadMessage()
		if err != nil || mt != websocket.BinaryMessage {
			t.Fatal("no UDP reply")
		}
		if len(f) < 14+40+8 || f[12] != 0x86 || f[13] != 0xDD {
			continue
		}
		ip := f[14:]
		if ip[0]>>4 != 6 || ip[6] != 17 {
			continue
		}
		ln := int(binary.BigEndian.Uint16(ip[4:6]))
		uh := ip[40 : 40+ln]
		if ip6UpperSum(ip[8:24], ip[24:40], 17, uh) != 0 {
			t.Fatal("bad UDP checksum")
		}
		if binary.BigEndian.Uint16(uh[0:2]) != uint16(uaddr.Port) || binary.BigEndian.Uint16(uh[2:4]) != 50001 {
			continue
		}
		if string(uh[8:]) != "ping-nat64" {
			t.Fatalf("bad echo %q", uh[8:])
		}
		if !net.IP(ip[8:24]).Equal(n6WKP) || !net.IP(ip[24:40]).Equal(n6ULA) {
			t.Fatalf("bad UDP addrs %v -> %v", net.IP(ip[8:24]), net.IP(ip[24:40]))
		}
		return
	}
}

func TestNAT64Passthrough(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	room := &Room{Clients: make(map[*Client]bool), Ctx: ctx, Cancel: cancel, MacToIP: make(map[string]net.IP)}
	room.NAT64 = newNat64Engine(ctx.Done())
	// On-link / LL / multicast / non-WKP GUA: untouched.
	cases := []net.IP{
		net.ParseIP("fd00:4::9").To16(),
		net.ParseIP("fe80::1").To16(),
		net.ParseIP("ff02::1").To16(),
		net.ParseIP("2001:db8::1").To16(),
	}
	for _, dst := range cases {
		f := mkIP6(t, gwMAC6, n6ULA, dst, 6, make([]byte, 20))
		if handleNAT64(f, nil, room) {
			t.Fatalf("should not translate dst %v", dst)
		}
	}
	// ICMP echo to WKP: not translated (no unprivileged raw socket).
	echo := append([]byte{128, 0, 0, 0, 0, 0, 0, 0}, []byte("0123456789ABCDEF")...)
	f := mkIP6(t, gwMAC6, n6ULA, n6WKP, 58, echo)
	if handleNAT64(f, nil, room) {
		t.Fatal("ICMP echo must fall through")
	}
}

// --- DNS64 ---

func startFakeDNS(t *testing.T) (string, func()) {
	t.Helper()
	mux := dns.NewServeMux()
	mux.HandleFunc("a-only.test.", func(w dns.ResponseWriter, r *dns.Msg) {
		m := new(dns.Msg)
		m.SetReply(r)
		m.Answer = append(m.Answer, &dns.A{
			Hdr: dns.RR_Header{Name: "a-only.test.", Rrtype: dns.TypeA, Class: dns.ClassINET, Ttl: 300},
			A:   net.ParseIP("192.0.2.1").To4(),
		})
		w.WriteMsg(m)
	})
	mux.HandleFunc("both.test.", func(w dns.ResponseWriter, r *dns.Msg) {
		m := new(dns.Msg)
		m.SetReply(r)
		for _, q := range r.Question {
			switch q.Qtype {
			case dns.TypeA:
				m.Answer = append(m.Answer, &dns.A{
					Hdr: dns.RR_Header{Name: "both.test.", Rrtype: dns.TypeA, Class: dns.ClassINET, Ttl: 300},
					A:   net.ParseIP("192.0.2.2").To4(),
				})
			case dns.TypeAAAA:
				m.Answer = append(m.Answer, &dns.AAAA{
					Hdr:  dns.RR_Header{Name: "both.test.", Rrtype: dns.TypeAAAA, Class: dns.ClassINET, Ttl: 300},
					AAAA: net.ParseIP("2001:db8::2").To16(),
				})
			}
		}
		w.WriteMsg(m)
	})
	mux.HandleFunc("highttl.test.", func(w dns.ResponseWriter, r *dns.Msg) {
		m := new(dns.Msg)
		m.SetReply(r)
		m.Answer = append(m.Answer, &dns.A{
			Hdr: dns.RR_Header{Name: "highttl.test.", Rrtype: dns.TypeA, Class: dns.ClassINET, Ttl: 3600},
			A:   net.ParseIP("192.0.2.9").To4(),
		})
		w.WriteMsg(m)
	})
	pc, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	srv := &dns.Server{PacketConn: pc, Handler: mux}
	go srv.ActivateAndServe()
	return pc.LocalAddr().String(), func() {
		testDNSServer = ""
		srv.Shutdown()
	}
}

func dnsQueryFrame(t *testing.T, name string, qtype uint16, sport uint16) []byte {
	t.Helper()
	m := new(dns.Msg)
	m.SetQuestion(name, qtype)
	q, err := m.Pack()
	if err != nil {
		t.Fatal(err)
	}
	return mkUDPseg(t, n6ULA, gwULA, sport, 53, q)
}

func readDNSReply(t *testing.T, c *websocket.Conn, sport uint16, timeout time.Duration) *dns.Msg {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		f := wsRead(t, c, time.Until(deadline))
		if f == nil {
			t.Fatal("no DNS reply")
		}
		if len(f) < 14+40+8 || f[12] != 0x86 || f[13] != 0xDD {
			continue
		}
		ip := f[14:]
		if ip[0]>>4 != 6 || ip[6] != 17 {
			continue
		}
		ln := int(binary.BigEndian.Uint16(ip[4:6]))
		uh := ip[40 : 40+ln]
		if ip6UpperSum(ip[8:24], ip[24:40], 17, uh) != 0 {
			t.Fatal("bad DNS UDP checksum")
		}
		if binary.BigEndian.Uint16(uh[0:2]) != 53 || binary.BigEndian.Uint16(uh[2:4]) != sport {
			continue
		}
		if !net.IP(ip[8:24]).Equal(gwULA) || !net.IP(ip[24:40]).Equal(n6ULA) {
			continue
		}
		m := new(dns.Msg)
		if err := m.Unpack(uh[8:]); err != nil {
			t.Fatal(err)
		}
		return m
	}
	t.Fatal("timeout")
	return nil
}

func TestDNS64Synthesize(t *testing.T) {
	url, _, cleanup := withNAT64Server(t)
	defer cleanup()
	addr, stopDNS := startFakeDNS(t)
	defer stopDNS()
	testDNSServer = addr
	defer func() { testDNSServer = "" }()

	c := wsDial(t, url)
	defer c.Close()

	if err := c.WriteMessage(websocket.BinaryMessage, dnsQueryFrame(t, "a-only.test.", dns.TypeAAAA, 53001)); err != nil {
		t.Fatal(err)
	}
	resp := readDNSReply(t, c, 53001, 10*time.Second)
	if resp.Rcode != dns.RcodeSuccess {
		t.Fatalf("rcode=%d", resp.Rcode)
	}
	if len(resp.Answer) != 1 {
		t.Fatalf("want 1 synthesized AAAA, got %d", len(resp.Answer))
	}
	a, ok := resp.Answer[0].(*dns.AAAA)
	if !ok {
		t.Fatalf("not AAAA: %v", resp.Answer[0])
	}
	want := append(append([]byte{}, nat64Prefix[:12]...), 192, 0, 2, 1)
	if !a.AAAA.Equal(net.IP(want)) {
		t.Fatalf("bad synthesized addr %v", a.AAAA)
	}
	if a.Hdr.Ttl != 300 {
		t.Fatalf("bad TTL %d", a.Hdr.Ttl)
	}
}

func TestDNS64Passthrough(t *testing.T) {
	url, _, cleanup := withNAT64Server(t)
	defer cleanup()
	addr, stopDNS := startFakeDNS(t)
	defer stopDNS()
	testDNSServer = addr
	defer func() { testDNSServer = "" }()

	c := wsDial(t, url)
	defer c.Close()

	// AAAA exists upstream: proxied verbatim, no synthesis.
	if err := c.WriteMessage(websocket.BinaryMessage, dnsQueryFrame(t, "both.test.", dns.TypeAAAA, 53002)); err != nil {
		t.Fatal(err)
	}
	resp := readDNSReply(t, c, 53002, 10*time.Second)
	if len(resp.Answer) != 1 {
		t.Fatalf("want 1 answer, got %d", len(resp.Answer))
	}
	a, ok := resp.Answer[0].(*dns.AAAA)
	if !ok || !a.AAAA.Equal(net.ParseIP("2001:db8::2")) {
		t.Fatalf("not proxied: %v", resp.Answer)
	}

	// A queries proxy as A records.
	if err := c.WriteMessage(websocket.BinaryMessage, dnsQueryFrame(t, "a-only.test.", dns.TypeA, 53003)); err != nil {
		t.Fatal(err)
	}
	resp = readDNSReply(t, c, 53003, 10*time.Second)
	if len(resp.Answer) != 1 {
		t.Fatalf("want 1 A answer, got %d", len(resp.Answer))
	}
	if _, ok := resp.Answer[0].(*dns.A); !ok {
		t.Fatalf("not A: %v", resp.Answer[0])
	}
}

func TestDNS64TTLClamp(t *testing.T) {
	url, _, cleanup := withNAT64Server(t)
	defer cleanup()
	addr, stopDNS := startFakeDNS(t)
	defer stopDNS()
	testDNSServer = addr
	defer func() { testDNSServer = "" }()

	c := wsDial(t, url)
	defer c.Close()

	if err := c.WriteMessage(websocket.BinaryMessage, dnsQueryFrame(t, "highttl.test.", dns.TypeAAAA, 53004)); err != nil {
		t.Fatal(err)
	}
	resp := readDNSReply(t, c, 53004, 10*time.Second)
	if len(resp.Answer) != 1 {
		t.Fatalf("want 1 answer, got %d", len(resp.Answer))
	}
	a, ok := resp.Answer[0].(*dns.AAAA)
	if !ok {
		t.Fatalf("not AAAA: %v", resp.Answer[0])
	}
	if a.Hdr.Ttl != 600 {
		t.Fatalf("TTL not clamped: %d", a.Hdr.Ttl)
	}
	want := append(append([]byte{}, nat64Prefix[:12]...), 192, 0, 2, 9)
	if !a.AAAA.Equal(net.IP(want)) {
		t.Fatalf("bad synthesized addr %v", a.AAAA)
	}
}
