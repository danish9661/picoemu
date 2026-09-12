package main

import (
	"context"
	"crypto/rand"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"sync"
	"time"

	"github.com/containers/gvisor-tap-vsock/pkg/types"
	"github.com/containers/gvisor-tap-vsock/pkg/virtualnetwork"
	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool {
		return true // Allow all origins for the local gateway
	},
}

type Client struct {
	Conn       *websocket.Conn
	WriteMutex sync.Mutex
}

type Room struct {
	sync.Mutex
	SessionId string
	VN        *virtualnetwork.VirtualNetwork
	Clients   map[*Client]bool
	PipeToVN  net.Conn
	Ctx       context.Context
	Cancel    context.CancelFunc
	NextIP    byte
	MacToIP   map[string]net.IP
}

var (
	roomsMutex sync.Mutex
	rooms      = make(map[string]*Room)
	globalVN   *virtualnetwork.VirtualNetwork
)

const PORT = "5090"

// gatewayPort allows running multiple gateway instances side by side
// (e.g. GATEWAY_PORT=5090); defaults to PORT.
func gatewayPort() string {
	if p := os.Getenv("GATEWAY_PORT"); p != "" {
		return p
	}
	return PORT
}

func main() {
	fmt.Println("===================================================")
	fmt.Println("   OpenHW Studio - Private IoT Gateway (Go)")
	fmt.Println("===================================================")

	http.HandleFunc("/api/network-gateway", func(w http.ResponseWriter, r *http.Request) {
		bridgeMutex.Lock()
		if bridgeEnabled {
			if bridgeTAP == nil {
				bt, err := createBridgeTAP(context.Background())
				if err != nil {
					bridgeMutex.Unlock()
					fmt.Printf("[Bridge] Failed to create TAP: %v\n", err)
					http.Error(w, "Failed to create TAP interface. On Windows, install the TAP driver from OpenVPN.", http.StatusInternalServerError)
					return
				}
				bridgeTAP = bt
			}
			bt := bridgeTAP
			bridgeMutex.Unlock()

			wsConn, err := upgrader.Upgrade(w, r, nil)
			if err != nil {
				fmt.Printf("[Bridge] WS upgrade error: %v\n", err)
				return
			}

			client := &Client{Conn: wsConn}
			bt.mutex.Lock()
			bt.clients[client] = true
			bt.mutex.Unlock()

			handleBridgeClient(client, bt)
			return
		}
		bridgeMutex.Unlock()

		sessionId := r.URL.Query().Get("sessionId")
		if sessionId == "" {
			b := make([]byte, 8)
			_, _ = rand.Read(b)
			sessionId = fmt.Sprintf("isolated-%x", b)
		}

		roomsMutex.Lock()
		room, exists := rooms[sessionId]
		if !exists {
			fmt.Printf("[Network Gateway] Creating new Virtual Network room: %s\n", sessionId)
			
			gatewayMode := os.Getenv("GATEWAY_MODE")
			var currentVN *virtualnetwork.VirtualNetwork

			if gatewayMode != "public" {
				if globalVN == nil {
					config := types.Configuration{
						Debug:             false,
						MTU:               1500,
						Subnet:            "192.168.4.0/24",
						GatewayIP:         "192.168.4.1",
						GatewayMacAddress: "5a:94:ef:e4:0c:dd",
					}

					vn, err := virtualnetwork.New(&config)
					if err != nil {
						roomsMutex.Unlock()
						fmt.Printf("Error creating virtual network: %v\n", err)
						return
					}
					globalVN = vn
				}
				currentVN = globalVN
			} else {
				config := types.Configuration{
					Debug:             false,
					MTU:               1500,
					Subnet:            "192.168.4.0/24",
					GatewayIP:         "192.168.4.1",
					GatewayMacAddress: "5a:94:ef:e4:0c:dd",
				}

				vn, err := virtualnetwork.New(&config)
				if err != nil {
					roomsMutex.Unlock()
					fmt.Printf("Error creating virtual network: %v\n", err)
					return
				}
				currentVN = vn
			}

			pipe1, pipe2, err := connLoopback()
			if err != nil {
				roomsMutex.Unlock()
				fmt.Printf("Error creating pipe: %v\n", err)
				return
			}

			ctx, cancel := context.WithCancel(context.Background())

			room = &Room{
				SessionId: sessionId,
				VN:        currentVN,
				Clients:   make(map[*Client]bool),
				PipeToVN:  pipe2,
				Ctx:       ctx,
				Cancel:    cancel,
				NextIP:    2,
				MacToIP:   make(map[string]net.IP),
			}
			rooms[sessionId] = room

			// Start the single gVisor acceptor for this room
			go currentVN.AcceptQemu(ctx, pipe1)

			// Start the single reader that takes frames from gVisor and broadcasts to ALL clients
			go gvisorToClientsLoop(room)

		} else {
			fmt.Printf("[Network Gateway] Joining existing Virtual Network room: %s\n", sessionId)
		}
		roomsMutex.Unlock()

		fmt.Printf("\n--- INCOMING WEBSOCKET REQUEST ---\n")
		fmt.Printf("URL: %s\n", r.URL.String())

		wsConn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			fmt.Printf("WebSocket Upgrade Error: %v\n", err)
			return
		}

		fmt.Println("[Network Gateway] Client connected. Upgrade successful!")

		client := &Client{Conn: wsConn}
		room.Lock()
		room.Clients[client] = true
		room.Unlock()

		handleClient(client, room)
	})

	listenAddr := "127.0.0.1:" + gatewayPort()
	if os.Getenv("GATEWAY_MODE") == "public" {
		listenAddr = ":" + gatewayPort()
	}

	// Start terminal command loop for bridge mode control
	go startCommandLoop()

	http.HandleFunc("/api/ble-gateway", handleBLEGateway)
	http.HandleFunc("/api/thread-gateway", handleThreadGateway)

	fmt.Printf("[Network Gateway] Server running on ws://%s/api/network-gateway\n", listenAddr)
	if err := http.ListenAndServe(listenAddr, nil); err != nil {
		fmt.Printf("Server Error: %v\n", err)
	}
}

func connLoopback() (net.Conn, net.Conn, error) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, nil, err
	}
	port := listener.Addr().(*net.TCPAddr).Port
	conn, err := net.Dial("tcp", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		return nil, nil, err
	}
	conn2, err := listener.Accept()
	listener.Close()
	return conn, conn2, err
}

// gvisorToClientsLoop reads Ethernet frames from gVisor and broadcasts them to all connected clients in the room.
func gvisorToClientsLoop(room *Room) {
	for {
		var length uint32
		err := binary.Read(room.PipeToVN, binary.BigEndian, &length)
		if err != nil {
			fmt.Printf("[Room %s] Pipe Read Error (size): %v\n", room.SessionId, err)
			return
		}

		buf := make([]byte, length)
		_, err = io.ReadFull(room.PipeToVN, buf)
		if err != nil {
			fmt.Printf("[Room %s] Pipe Read Error (data): %v\n", room.SessionId, err)
			return
		}

		if len(buf) >= 14 {
			dst := buf[0:6]
			src := buf[6:12]
			ethType := binary.BigEndian.Uint16(buf[12:14])
			fmt.Printf("[%s] [gVisor -> Hub] << Eth Frame (dst=%x, src=%x, type=0x%04x, len=%d)\n", time.Now().Format("15:04:05.000"), dst, src, ethType, length)
		}

		// Broadcast frame to all clients
		room.Lock()
		targets := make([]*Client, 0, len(room.Clients))
		for client := range room.Clients {
			targets = append(targets, client)
		}
		room.Unlock()

		for _, client := range targets {
			client.WriteMutex.Lock()
			err = client.Conn.WriteMessage(websocket.BinaryMessage, buf)
			client.WriteMutex.Unlock()
			if err != nil {
				fmt.Printf("[Room %s] Client Write Error: %v\n", room.SessionId, err)
			}
		}
	}
}

// handleClient reads Ethernet frames from a specific WebSocket client and multiplexes them.
func handleClient(client *Client, room *Room) {
	defer func() {
		client.Conn.Close()
		room.Lock()
		delete(room.Clients, client)
		isEmpty := len(room.Clients) == 0
		room.Unlock()
		fmt.Println("[Network Gateway] Client disconnected.")

		if isEmpty {
			fmt.Printf("[Room %s] Empty! Cleaning up virtual network...\n", room.SessionId)
			room.Cancel() // Stop vn.AcceptQemu
			if room.PipeToVN != nil {
				room.PipeToVN.Close() // Stop gvisorToClientsLoop
			}

			roomsMutex.Lock()
			delete(rooms, room.SessionId)
			roomsMutex.Unlock()
		}
	}()

	// Ping/Pong Timeout Setup
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
			case <-room.Ctx.Done():
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
		messageType, msg, err := client.Conn.ReadMessage()
		if err != nil {
			fmt.Printf("WebSocket Read Error: %v\n", err)
			return
		}
		// Any inbound traffic (data or Ping) proves liveness; keep the
		// read deadline rolling so quiet-but-connected clients survive.
		client.Conn.SetReadDeadline(time.Now().Add(pongWait))

		if messageType == websocket.BinaryMessage {
			if len(msg) >= 14 {
				dst := msg[0:6]
				src := msg[6:12]
				ethType := binary.BigEndian.Uint16(msg[12:14])
				fmt.Printf("[%s] [ESP32 -> Hub] >> Eth Frame (dst=%x, src=%x, type=0x%04x, len=%d)\n", time.Now().Format("15:04:05.000"), dst, src, ethType, len(msg))
			}

			// Intercept DHCP Packets
			packet := gopacket.NewPacket(msg, layers.LayerTypeEthernet, gopacket.Default)
			if udpLayer := packet.Layer(layers.LayerTypeUDP); udpLayer != nil {
				udp, _ := udpLayer.(*layers.UDP)
				if udp.DstPort == 67 {
					handleDHCP(msg, packet, client, room)
					continue // DO NOT forward to gVisor or other clients!
				}
			}

			// Intercept IPv6 NDP/ICMPv6 for gateway addresses (RA/NA/echo).
			// Handled frames go straight back to the requester; the rest
			// fall through to room broadcast + gVisor like before.
			if len(msg) >= 14 && binary.BigEndian.Uint16(msg[12:14]) == 0x86DD {
				if handleICMPv6(msg, client) {
					continue
				}
			}

			// 1. Send frame to gVisor stack
			room.Lock()
			pipe := room.PipeToVN
			room.Unlock()
			
			if pipe != nil {
				err = binary.Write(pipe, binary.BigEndian, uint32(len(msg)))
				if err == nil {
					_, err = pipe.Write(msg)
				}
			}
			
			// 2. Broadcast frame to all *other* clients (Layer 2 Hub logic)
			room.Lock()
			targets := make([]*Client, 0, len(room.Clients))
			for otherClient := range room.Clients {
				if otherClient != client {
					targets = append(targets, otherClient)
				}
			}
			room.Unlock()

			for _, otherClient := range targets {
				otherClient.WriteMutex.Lock()
				otherClient.Conn.WriteMessage(websocket.BinaryMessage, msg)
				otherClient.WriteMutex.Unlock()
			}

			if err != nil {
				fmt.Printf("[Room %s] Pipe Write Error: %v\n", room.SessionId, err)
				return
			}
		}
	}
}

// handleBLEGateway proxies BLE HCI packets between browser WS and Bumble TCP
func handleBLEGateway(w http.ResponseWriter, r *http.Request) {
	fmt.Printf("[BLE Gateway] Incoming connection request from %s\n", r.RemoteAddr)
	wsConn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		fmt.Printf("[BLE] WS upgrade error: %v\n", err)
		return
	}
	defer wsConn.Close()

	fmt.Println("[BLE Gateway] Browser client connected. Dialing Bumble TCP server (localhost:9544)...")
	tcpConn, err := net.DialTimeout("tcp", "127.0.0.1:9544", 5*time.Second)
	if err != nil {
		fmt.Printf("[BLE Gateway] Failed to connect to Bumble (port 9544): %v. Please make sure vhci_bridge.py is running.\n", err)
		wsConn.WriteMessage(websocket.TextMessage, []byte("ERROR: Failed to connect to Bumble (port 9544). Ensure vhci_bridge.py is running."))
		return
	}
	defer tcpConn.Close()
	fmt.Println("[BLE Gateway] Connected to Bumble TCP server successfully!")

	errChan := make(chan error, 2)

	// WS -> TCP
	go func() {
		for {
			messageType, msg, err := wsConn.ReadMessage()
			if err != nil {
				errChan <- err
				return
			}
			if messageType == websocket.BinaryMessage {
				_, err = tcpConn.Write(msg)
				if err != nil {
					errChan <- err
					return
				}
			}
		}
	}()

	// TCP -> WS
	go func() {
		buf := make([]byte, 4096)
		for {
			n, err := tcpConn.Read(buf)
			if err != nil {
				errChan <- err
				return
			}
			if n > 0 {
				err = wsConn.WriteMessage(websocket.BinaryMessage, buf[:n])
				if err != nil {
					errChan <- err
					return
				}
			}
		}
	}()

	err = <-errChan
	fmt.Printf("[BLE Gateway] Connection closed: %v\n", err)
}

type ThreadClient struct {
	Conn       *websocket.Conn
	WriteMutex sync.Mutex
}

type ThreadRoom struct {
	sync.Mutex
	SessionId string
	Clients   map[*ThreadClient]bool
}

var (
	threadRoomsMutex sync.Mutex
	threadRooms      = make(map[string]*ThreadRoom)
)

// handleThreadGateway routes IEEE 802.15.4 frames between C6 devices in the same session
func handleThreadGateway(w http.ResponseWriter, r *http.Request) {
	sessionId := r.URL.Query().Get("sessionId")
	if sessionId == "" {
		sessionId = "default"
	}

	wsConn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		fmt.Printf("[Thread] WS upgrade error: %v\n", err)
		return
	}

	client := &ThreadClient{Conn: wsConn}

	threadRoomsMutex.Lock()
	room, exists := threadRooms[sessionId]
	if !exists {
		room = &ThreadRoom{
			SessionId: sessionId,
			Clients:   make(map[*ThreadClient]bool),
		}
		threadRooms[sessionId] = room
		fmt.Printf("[Thread Gateway] Created new room: %s\n", sessionId)
	}
	room.Lock()
	room.Clients[client] = true
	room.Unlock()
	threadRoomsMutex.Unlock()

	fmt.Printf("[Thread Gateway] Client joined room %s\n", sessionId)

	defer func() {
		wsConn.Close()
		room.Lock()
		delete(room.Clients, client)
		isEmpty := len(room.Clients) == 0
		room.Unlock()

		if isEmpty {
			threadRoomsMutex.Lock()
			delete(threadRooms, sessionId)
			threadRoomsMutex.Unlock()
			fmt.Printf("[Thread Gateway] Cleaned up room %s\n", sessionId)
		}
	}()

	for {
		messageType, msg, err := wsConn.ReadMessage()
		if err != nil {
			return
		}

		if messageType == websocket.BinaryMessage {
			room.Lock()
			targets := make([]*ThreadClient, 0, len(room.Clients))
			for other := range room.Clients {
				if other != client {
					targets = append(targets, other)
				}
			}
			room.Unlock()

			for _, other := range targets {
				other.WriteMutex.Lock()
				other.Conn.WriteMessage(websocket.BinaryMessage, msg)
				other.WriteMutex.Unlock()
			}
		}
	}
}
