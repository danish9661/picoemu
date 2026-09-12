# OpenHW Network Gateway 🌐

This is the standalone **SLIRP/NAT Proxy & Multiplayer Bridge** for OpenHW Studio, written in **Go** using Google's `gVisor` virtual network stack! It bridges the gap between the browser-based ESP32 simulation sandbox and your native operating system's networking stack.

## 🚀 How it Works
When a user toggles the **"Private IoT Gateway"** in the OpenHW Studio UI, the simulated ESP32 stops routing its raw 802.11 Wi-Fi frames to the public cloud. Instead, it forwards them over a WebSocket (`ws://localhost:5099`) directly to this script running on your local machine.

This Go application feeds those raw Ethernet frames directly into `gvisor-tap-vsock`. gVisor acts exactly like a real hardware router — it terminates the virtual TCP/IP connection and establishes real operating system network sockets to fulfill the requests!

---

## 🛠️ Features (Fully Completed)

Because this uses a battle-tested network stack, **everything works out of the box**:
- ✅ **Full TCP Sequence Machine:** HTTP GET/POST, WebSockets, MQTT.
- ✅ **HTTPS (SSL/TLS):** Transparently supported.
- ✅ **DNS Resolution:** UDP Port 53 queries are automatically handled.
- ✅ **DHCP:** Built-in DHCP server assigns IP addresses to the ESP32.
- ✅ **Zero Config:** Users don't need to install libpcap, npcap, or set up TAP devices manually.
- ✅ **🎮 Multiplayer Rooms:** You can bridge multiple ESP32s together over the internet! By entering a Room Code in the F1 Menu, the Gateway dynamically places multiple WebSockets into the exact same Virtual Router so your boards can talk to each other!
- ✅ **🌍 Inbound Web Servers (Port Forwarding):** By default in private mode, the Gateway automatically forwards `localhost:8080` to the simulated ESP32's IP address on port `80`. This allows you to host web servers on the ESP32 and access them securely from your browser!

---

## 🔒 Security & GATEWAY_MODE

To protect the production infrastructure from arbitrary port-forwarding exposure, the gateway supports a `GATEWAY_MODE` environment variable.
- **Private Mode (Default):** When you run the gateway locally (or `GATEWAY_MODE` is empty/not set to "public"), it automatically enables inbound port forwarding (`localhost:8080` -> `ESP32:80`).
- **Public Mode:** When deployed to the cloud (by setting `GATEWAY_MODE=public` in `docker-compose.prod.yml`), port forwarding is strictly **disabled** to prevent collisions and protect the server. Outbound connections (like to Blynk or Firebase) still work perfectly!

---

## 💻 How to Use (Local Developers)

We designed this so that local developers do **not** necessarily need Go installed!

### For Windows Users
Simply double-click the `start-gateway.cmd` file! 
1. If you have Go installed, it will compile it natively.
2. If you don't have Go but you have Docker installed, it will compile it inside a temporary Docker container!

### For Mac / Linux Users
Run the shell script:
```bash
./start-gateway.sh
```

Keep the terminal window open while using the OpenHW Studio simulator to view live connection logs!

---

## 📦 Building Single Executables for All Systems

Go makes it incredibly easy to compile single, standalone executables for any operating system. These executables bundle the entire gateway into one `.exe` or binary file with zero dependencies! This is perfect for distributing the Private Gateway to students who don't have Go or Docker installed.

If you have Go installed, you can cross-compile the gateway for different operating systems from your terminal:

**1. Build for Windows (Executable):**
```bash
GOOS=windows GOARCH=amd64 go build -o openhw-gateway-windows.exe main.go
```
*Users just double-click this `.exe` and the gateway starts instantly on port 5099.*

**2. Build for macOS (Intel):**
```bash
GOOS=darwin GOARCH=amd64 go build -o openhw-gateway-macos-intel main.go
```

**3. Build for macOS (Apple Silicon M1/M2/M3):**
```bash
GOOS=darwin GOARCH=arm64 go build -o openhw-gateway-macos-arm main.go
```

**4. Build for Linux:**
```bash
GOOS=linux GOARCH=amd64 go build -o openhw-gateway-linux main.go
```

*Mac and Linux users simply open their terminal, run `chmod +x <binary_name>`, and then execute it like `./openhw-gateway-linux`.*

---

## 🐳 Docker Deployment (Cloud Public Gateway)
If you are deploying this for the public `api.openhw-studio.com` gateway, our `Dockerfile` uses a **multi-stage build**. It compiles the Go binary using `golang:alpine` and then ships it inside a tiny 15MB `alpine` base image. No Node.js or Go runtime is required in production!
