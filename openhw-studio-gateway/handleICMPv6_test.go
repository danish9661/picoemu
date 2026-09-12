package main

import (
	"encoding/binary"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

func cksum(b []byte) uint16 {
	s := uint32(0)
	for i := 0; i+1 < len(b); i += 2 {
		s += uint32(b[i])<<8 | uint32(b[i+1])
	}
	if len(b)&1 != 0 {
		s += uint32(b[len(b)-1]) << 8
	}
	for s>>16 != 0 {
		s = (s & 0xFFFF) + (s >> 16)
	}
	return uint16(^s & 0xFFFF)
}

func ticmp6Sum(src, dst net.IP, pl []byte) uint16 {
	pseudo := append(append([]byte{}, src.To16()...), dst.To16()...)
	ln := make([]byte, 4)
	binary.BigEndian.PutUint32(ln, uint32(len(pl)))
	pseudo = append(append(pseudo, ln...), 0, 0, 0, 58)
	return cksum(append(pseudo, pl...))
}

var (
	tMyMAC = net.HardwareAddr{0x02, 0x12, 0x34, 0x56, 0x78, 0x02}
	tMyIP  = net.ParseIP("fe80::2").To16()
	tGwMAC = net.HardwareAddr{0x5a, 0x94, 0xef, 0xe4, 0x0c, 0xdd}
	tGwULA = net.ParseIP("fd00:4::1").To16()
)

func mkFrame(t *testing.T, dstMAC net.HardwareAddr, srcIP, dstIP net.IP, body []byte) []byte {
	t.Helper()
	f := make([]byte, 14+40+len(body))
	copy(f[0:], dstMAC)
	copy(f[6:], tMyMAC)
	f[12], f[13] = 0x86, 0xDD
	f[14] = 0x60
	binary.BigEndian.PutUint16(f[18:], uint16(len(body)))
	f[20], f[21] = 58, 64
	copy(f[22:], srcIP.To16())
	copy(f[38:], dstIP.To16())
	copy(f[54:], body)
	cs := icmp6Sum(srcIP, dstIP, body)
	binary.BigEndian.PutUint16(f[56:], cs)
	return f
}

func withServer(t *testing.T) (string, func()) {
	t.Helper()
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		c, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer c.Close()
		cl := &Client{Conn: c}
		for {
			mt, msg, err := c.ReadMessage()
			if err != nil {
				return
			}
			if mt == websocket.BinaryMessage {
				handleICMPv6(msg, cl)
			}
		}
	}))
	return "ws" + strings.TrimPrefix(srv.URL, "http"), srv.Close
}

func wsDial(t *testing.T, url string) *websocket.Conn {
	t.Helper()
	c, _, err := websocket.DefaultDialer.Dial(url, nil)
	if err != nil {
		t.Fatal(err)
	}
	return c
}

func wsRead(t *testing.T, c *websocket.Conn, timeout time.Duration) []byte {
	t.Helper()
	c.SetReadDeadline(time.Now().Add(timeout))
	mt, msg, err := c.ReadMessage()
	if err != nil {
		return nil
	}
	if mt != websocket.BinaryMessage {
		return nil
	}
	return msg
}

func TestRStoRA(t *testing.T) {
	url, done := withServer(t)
	defer done()
	c := wsDial(t, url)
	defer c.Close()

	// RS with unspecified source -> multicast RA
	body := []byte{133, 0, 0, 0, 0, 0, 0, 0}
	all := net.ParseIP("ff02::2").To16()
	allMAC := net.HardwareAddr{0x33, 0x33, 0, 0, 0, 2}
	if err := c.WriteMessage(websocket.BinaryMessage, mkFrame(t, allMAC, tMyIP, all, body)); err != nil {
		t.Fatal(err)
	}
	f := wsRead(t, c, 5*time.Second)
	if f == nil {
		t.Fatal("no RA received")
	}
	ip := f[14:]
	seg := ip[40:]
	if len(seg) < 64 || seg[0] != 134 {
		t.Fatalf("not an RA: type=%d len=%d", seg[0], len(seg))
	}
	if icmp6Sum(ip[8:24], ip[24:40], seg) != 0 {
		t.Fatal("bad RA checksum")
	}
	// prefix option at +24: type 3, /64, L+A, fd00:4::/64 at +40
	if seg[24] != 3 || seg[26] != 64 || seg[27] != 0xC0 {
		t.Fatalf("bad prefix option: %x", seg[24:28])
	}
	if string(seg[40:48]) != string([]byte{0xFD, 0, 0, 4, 0, 0, 0, 0}) {
		t.Fatalf("bad prefix: %x", seg[40:48])
	}
}

func TestNStoNA(t *testing.T) {
	url, done := withServer(t)
	defer done()
	c := wsDial(t, url)
	defer c.Close()

	snMAC := net.HardwareAddr{0x33, 0x33, 0xFF, 0, 0, 1}
	snIP := net.ParseIP("ff02::1:ff00:1").To16()
	ns := append([]byte{135, 0, 0, 0, 0, 0, 0, 0}, tGwULA...)
	ns = append(ns, 1, 1)
	ns = append(ns, tMyMAC...)
	if err := c.WriteMessage(websocket.BinaryMessage, mkFrame(t, snMAC, tMyIP, snIP, ns)); err != nil {
		t.Fatal(err)
	}
	f := wsRead(t, c, 5*time.Second)
	if f == nil {
		t.Fatal("no NA received")
	}
	ip := f[14:]
	seg := ip[40:]
	if len(seg) < 32 || seg[0] != 136 || seg[4]&0x60 != 0x60 {
		t.Fatalf("not a valid NA: %x", seg[:8])
	}
	if string(seg[8:24]) != string(tGwULA) {
		t.Fatalf("wrong NA target: %x", seg[8:24])
	}
	if seg[24] != 2 || seg[25] != 1 || string(seg[26:32]) != string(tGwMAC) {
		t.Fatalf("bad tgt-ll option: %x", seg[24:32])
	}
	if icmp6Sum(ip[8:24], ip[24:40], seg) != 0 {
		t.Fatal("bad NA checksum")
	}

	// NS for someone else -> silence
	other := net.ParseIP("fd00:4::99").To16()
	ns2 := append([]byte{135, 0, 0, 0, 0, 0, 0, 0}, other...)
	ns2 = append(ns2, 1, 1)
	ns2 = append(ns2, tMyMAC...)
	sn2 := net.ParseIP("ff02::1:ff00:99").To16()
	mac2 := net.HardwareAddr{0x33, 0x33, 0xFF, 0, 0, 0x99}
	if err := c.WriteMessage(websocket.BinaryMessage, mkFrame(t, mac2, tMyIP, sn2, ns2)); err != nil {
		t.Fatal(err)
	}
	if f := wsRead(t, c, 2*time.Second); f != nil {
		t.Fatal("unexpected reply to foreign NS")
	}
}

func TestEchoReply(t *testing.T) {
	url, done := withServer(t)
	defer done()
	c := wsDial(t, url)
	defer c.Close()

	echo := append([]byte{128, 0, 0, 0, 0x22, 0x22, 0, 7}, []byte("0123456789ABCDEF")...)
	if err := c.WriteMessage(websocket.BinaryMessage, mkFrame(t, tGwMAC, tMyIP, tGwULA, echo)); err != nil {
		t.Fatal(err)
	}
	f := wsRead(t, c, 5*time.Second)
	if f == nil {
		t.Fatal("no echo reply")
	}
	ip := f[14:]
	seg := ip[40:]
	if len(seg) < 24 || seg[0] != 129 || string(seg[4:8]) != "\x22\x22\x00\x07" {
		t.Fatalf("bad echo reply: %x", seg[:8])
	}
	if string(seg[8:24]) != "0123456789ABCDEF" {
		t.Fatal("payload not echoed")
	}
	if icmp6Sum(ip[8:24], ip[24:40], seg) != 0 {
		t.Fatal("bad echo checksum")
	}

	// Echo elsewhere -> silence
	echo2 := append([]byte{128, 0, 0, 0, 0, 0, 0, 0}, []byte("0123456789ABCDEF")...)
	other := net.ParseIP("fd00:4::99").To16()
	if err := c.WriteMessage(websocket.BinaryMessage, mkFrame(t, tGwMAC, tMyIP, other, echo2)); err != nil {
		t.Fatal(err)
	}
	if f := wsRead(t, c, 2*time.Second); f != nil {
		t.Fatal("unexpected reply to foreign echo")
	}
}

func TestShortFramesIgnored(t *testing.T) {
	url, done := withServer(t)
	defer done()
	c := wsDial(t, url)
	defer c.Close()

	for _, f := range [][]byte{
		{},
		{1, 2, 3},
		append(append([]byte{}, tMyMAC...), tGwMAC...),
		append([]byte{0x86, 0xDD}, make([]byte, 60)...),
	} {
		full := append(append([]byte{}, tGwMAC...), tMyMAC...)
		full = append(full, f...)
		if err := c.WriteMessage(websocket.BinaryMessage, full); err != nil {
			t.Fatal(err)
		}
	}
	if f := wsRead(t, c, 2*time.Second); f != nil {
		t.Fatal("unexpected reply to junk")
	}
}
