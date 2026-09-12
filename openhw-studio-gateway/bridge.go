package main

import (
	"bufio"
	"context"
	"encoding/binary"
	"fmt"
	"net"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
	"github.com/songgao/water"
)

var (
	bridgeEnabled bool
	bridgeTAP     *BridgeTAP
	bridgeMutex   sync.Mutex
)

type BridgeTAP struct {
	ifce    *water.Interface
	clients map[*Client]bool
	mutex   sync.Mutex
	ctx     context.Context
	cancel  context.CancelFunc
}

func startCommandLoop() {
	scanner := bufio.NewScanner(os.Stdin)
	fmt.Println("\n--- Gateway Commands ---")
	fmt.Println("  bridge enable   - Enable bridge mode (all new clients use TAP bridge)")
	fmt.Println("  bridge disable  - Disable bridge mode, new clients use gVisor instead")
	fmt.Println("  status          - Show gateway status")
	fmt.Println("  help            - Show this help")
	fmt.Println("------------------------")
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}
		parts := strings.Fields(line)
		if len(parts) == 0 {
			continue
		}
		switch parts[0] {
		case "bridge":
			if len(parts) < 2 {
				fmt.Println("Usage: bridge enable|disable")
				continue
			}
			switch parts[1] {
			case "enable":
				enableBridge()
			case "disable":
				disableBridge()
			default:
				fmt.Println("Usage: bridge enable|disable")
			}
		case "status":
			printStatus()
		case "help":
			fmt.Println("Commands: bridge enable|disable, status, help")
		default:
			fmt.Printf("Unknown command: %s\n", parts[0])
		}
	}
}

func enableBridge() {
	bridgeMutex.Lock()
	defer bridgeMutex.Unlock()
	if bridgeEnabled {
		fmt.Println("[Bridge] Already enabled")
		return
	}
	bridgeEnabled = true
	fmt.Println("[Bridge] Enabled. All new clients will use TAP bridge mode.")
	fmt.Println("[Bridge] Port forwarding is NOT needed — devices get a real LAN IP.")
	fmt.Println("[Bridge] Connect your ESP32 simulator now. The TAP will be created automatically.")
	fmt.Println("[Bridge] Then bridge the TAP to your LAN (setup instructions will appear).")
}

func disableBridge() {
	bridgeMutex.Lock()
	bridgeEnabled = false
	bt := bridgeTAP
	bridgeTAP = nil
	bridgeMutex.Unlock()

	if bt != nil {
		bt.mutex.Lock()
		for client := range bt.clients {
			client.Conn.Close()
		}
		bt.mutex.Unlock()
		bt.cleanup()
	}
	fmt.Println("[Bridge] Disabled")
}

func printStatus() {
	bridgeMutex.Lock()
	defer bridgeMutex.Unlock()
	fmt.Printf("Bridge mode: %v\n", bridgeEnabled)
	roomsMutex.Lock()
	fmt.Printf("Active rooms: %d\n", len(rooms))
	roomsMutex.Unlock()
	if bridgeTAP != nil {
		bridgeTAP.mutex.Lock()
		fmt.Printf("TAP interface: %s\n", bridgeTAP.ifce.Name())
		fmt.Printf("Bridge clients: %d\n", len(bridgeTAP.clients))
		bridgeTAP.mutex.Unlock()
	} else {
		fmt.Println("TAP interface: none")
	}
}

func createBridgeTAP(parentCtx context.Context) (*BridgeTAP, error) {
	ifce, err := water.New(water.Config{DeviceType: water.TAP})
	if err != nil {
		return nil, fmt.Errorf("create TAP: %w", err)
	}
	ctx, cancel := context.WithCancel(parentCtx)
	bt := &BridgeTAP{
		ifce:    ifce,
		clients: make(map[*Client]bool),
		ctx:     ctx,
		cancel:  cancel,
	}
	fmt.Printf("[Bridge] TAP interface created: %s\n", ifce.Name())
	fmt.Println("[Bridge] Port forwarding is disabled — device gets a real LAN IP.")
	fmt.Println("[Bridge] You MUST bridge the TAP to your LAN for the ESP32 to get an IP:")
	fmt.Println("[Bridge]   Windows: Network Settings -> 'More network adapter options'")
	fmt.Println("[Bridge]            Shift-select TAP + your WiFi/Ethernet -> right-click -> Bridge")
	fmt.Println("[Bridge]   Linux:   sudo ip link add br0 type bridge && sudo ip link set eth0 master br0")
	fmt.Println("[Bridge]            sudo ip link set tap0 master br0 && sudo ip link set dev br0 up")
	go bt.startReadLoop()
	return bt, nil
}

func (bt *BridgeTAP) startReadLoop() {
	frame := make([]byte, 1500)
	for {
		n, err := bt.ifce.Read(frame)
		if err != nil {
			select {
			case <-bt.ctx.Done():
				return
			default:
				fmt.Printf("[Bridge] TAP read error: %v\n", err)
				return
			}
		}

		bt.mutex.Lock()
		clients := make([]*Client, 0, len(bt.clients))
		for c := range bt.clients {
			clients = append(clients, c)
		}
		bt.mutex.Unlock()

		for _, c := range clients {
			c.WriteMutex.Lock()
			err = c.Conn.WriteMessage(websocket.BinaryMessage, frame[:n])
			c.WriteMutex.Unlock()
			if err != nil {
				fmt.Printf("[Bridge] Client write error: %v\n", err)
			}
		}
	}
}

func (bt *BridgeTAP) cleanup() {
	bt.mutex.Lock()
	defer bt.mutex.Unlock()
	if bt.ifce != nil {
		bt.cancel()
		bt.ifce.Close()
		bt.ifce = nil
		fmt.Println("[Bridge] TAP interface destroyed")
	}
}

func extractFrameIP(frame []byte) net.IP {
	if len(frame) < 14 {
		return nil
	}
	ethType := binary.BigEndian.Uint16(frame[12:14])
	switch ethType {
	case 0x0800: // IPv4
		if len(frame) < 30 {
			return nil
		}
		return net.IP(frame[26:30])
	case 0x0806: // ARP
		if len(frame) < 42 {
			return nil
		}
		return net.IP(frame[28:32])
	case 0x86DD: // IPv6: source address (for BOARD_IP display)
		if len(frame) < 38 {
			return nil
		}
		return net.IP(frame[22:38])
	}
	return nil
}

func handleBridgeClient(client *Client, bt *BridgeTAP) {
	clientAddr := client.Conn.RemoteAddr()
	fmt.Printf("[Bridge] Client connected: %s\n", clientAddr)

	discoveredIPs := make(map[string]bool)
	ipPrinted := false

	defer func() {
		client.Conn.Close()
		bt.mutex.Lock()
		delete(bt.clients, client)
		isEmpty := len(bt.clients) == 0
		bt.mutex.Unlock()

		if isEmpty {
			bt.cleanup()
			bridgeMutex.Lock()
			if bridgeTAP == bt {
				bridgeTAP = nil
			}
			bridgeMutex.Unlock()
		}
		fmt.Printf("[Bridge] Client disconnected: %s\n", clientAddr)
	}()

	pongWait := 60 * time.Second
	pingPeriod := 50 * time.Second

	client.Conn.SetReadDeadline(time.Now().Add(pongWait))
	client.Conn.SetPongHandler(func(string) error {
		client.Conn.SetReadDeadline(time.Now().Add(pongWait))
		return nil
	})

	clientCtx, clientCancel := context.WithCancel(context.Background())
	defer clientCancel()

	go func() {
		ticker := time.NewTicker(pingPeriod)
		defer ticker.Stop()
		for {
			select {
			case <-clientCtx.Done():
				return
			case <-bt.ctx.Done():
				client.Conn.Close()
				return
			case <-ticker.C:
				client.WriteMutex.Lock()
				err := client.Conn.WriteMessage(websocket.PingMessage, nil)
				client.WriteMutex.Unlock()
				if err != nil {
					client.Conn.Close()
					return
				}
			}
		}
	}()

	for {
		_, msg, err := client.Conn.ReadMessage()
		if err != nil {
			return
		}

		if ip := extractFrameIP(msg); ip != nil {
			ipStr := ip.String()
			if ipStr != "0.0.0.0" && !discoveredIPs[ipStr] {
				discoveredIPs[ipStr] = true
				fmt.Printf("[Bridge] Device IP detected: %s\n", ipStr)
				client.WriteMutex.Lock()
				client.Conn.WriteMessage(websocket.TextMessage, []byte("BOARD_IP:"+ipStr))
				client.WriteMutex.Unlock()
				if !ipPrinted {
					ipPrinted = true
					fmt.Printf("[Bridge] Access web server at http://%s\n", ipStr)
					fmt.Println("[Bridge] Port forwarding is disabled in bridge mode — device is directly on your LAN.")
				}
			}
		}

		_, err = bt.ifce.Write(msg)
		if err != nil {
			fmt.Printf("[Bridge] TAP write error: %v\n", err)
			return
		}
	}
}
