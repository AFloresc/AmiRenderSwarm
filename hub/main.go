package main

import (
	"bufio"
	"crypto/rand"
	"encoding/hex"
	"flag"
	"log"
	"net"
	"strconv"
	"strings"
	"time"
)

func main() {
	addr := flag.String("addr", ":7900", "Dirección TCP de escucha del hub")
	heartbeatTimeout := flag.Duration("timeout", 60*time.Second, "Tiempo máximo sin PING antes de expulsar un nodo")
	flag.Parse()

	registry := NewRegistry()

	// Limpiador periódico de nodos inactivos
	go func() {
		ticker := time.NewTicker(15 * time.Second)
		defer ticker.Stop()
		for range ticker.C {
			for _, id := range registry.SweepStale(*heartbeatTimeout) {
				log.Printf("[SWEEP] Agente desconectado por timeout: %s", id)
			}
		}
	}()

	ln, err := net.Listen("tcp", *addr)
	if err != nil {
		log.Fatalf("Error iniciando listener TCP en %s: %v", *addr, err)
	}
	defer ln.Close()

	log.Printf("AmiRenderSwarm Hub escuchando en %s", *addr)

	for {
		conn, err := ln.Accept()
		if err != nil {
			log.Printf("Error aceptando conexión: %v", err)
			continue
		}
		go handleConn(conn, registry)
	}
}

func handleConn(conn net.Conn, reg *Registry) {
	defer conn.Close()

	remoteIP, _, _ := net.SplitHostPort(conn.RemoteAddr().String())
	sendCh := make(chan string, 32)
	done := make(chan struct{})

	// Goroutine dedicada de escritura
	go func() {
		for {
			select {
			case line := <-sendCh:
				_ = conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
				if _, err := conn.Write([]byte(line)); err != nil {
					return
				}
			case <-done:
				return
			}
		}
	}()

	var current *Agent
	defer func() {
		close(done)
		if current != nil {
			reg.Unregister(current.ID, current.SessionID)
			log.Printf("[DISCONNECT] %s (%s)", current.ID, remoteIP)
		}
	}()

	scanner := bufio.NewScanner(conn)
	// Límite de 4 KB por línea de señalización
	scanner.Buffer(make([]byte, 4096), 4096)

	for scanner.Scan() {
		msg, err := ParseMessage(scanner.Text())
		if err != nil {
			sendCh <- FormatMessage("ERR", "code", "bad_syntax", "msg", err.Error())
			continue
		}

		if msg.Verb != "HELLO" && current == nil {
			sendCh <- FormatMessage("ERR", "code", "unregistered", "msg", "send_HELLO_first")
			continue
		}

		switch msg.Verb {
		case "HELLO":
			id := msg.Params["id"]
			if id == "" {
				sendCh <- FormatMessage("ERR", "code", "missing_id")
				continue
			}

			ramMB, _ := strconv.Atoi(msg.Params["ram_mb"])
			listenPort, _ := strconv.Atoi(msg.Params["listen_port"])
			if listenPort == 0 {
				listenPort = 7800
			}
			role := strings.ToLower(msg.Params["role"])
			if role == "" {
				role = "both"
			}

			var engines []string
			if rawEng := msg.Params["engines"]; rawEng != "" {
				for _, e := range strings.Split(rawEng, ",") {
					if trimmed := strings.TrimSpace(e); trimmed != "" {
						engines = append(engines, trimmed)
					}
				}
			}

			current = &Agent{
				ID:         id,
				SessionID:  newSessionID(),
				OS:         msg.Params["os"],
				CPU:        msg.Params["cpu"],
				FPU:        msg.Params["fpu"] == "1",
				RamMB:      ramMB,
				Engines:    engines,
				Role:       role,
				IP:         remoteIP,
				ListenPort: listenPort,
				State:      "IDLE",
				LastSeen:   time.Now(),
				Conn:       conn,
				SendCh:     sendCh,
			}

			reg.Register(current)
			log.Printf("[HELLO] %s ip=%s port=%d cpu=%s ram=%dMB engines=%s",
				current.ID, current.IP, current.ListenPort, current.CPU, current.RamMB, current.EnginesCSV())

			sendCh <- FormatMessage("WELCOME", "peer_ip", current.IP, "session", current.SessionID)

		case "PING":
			reg.UpdateHeartbeat(current.ID, msg.Params["state"], msg.Params["load"])
			sendCh <- FormatMessage("PONG", "ts", strconv.FormatInt(time.Now().Unix(), 10))

		case "STATE":
			if st := msg.Params["state"]; st != "" {
				reg.SetState(current.ID, st)
				sendCh <- FormatMessage("OK", "state", strings.ToUpper(st))
			}

		case "LIST_PEERS":
			minRAM, _ := strconv.Atoi(msg.Params["min_ram"])
			peers := reg.ListAvailable(current.ID, msg.Params["engine"], minRAM)
			for _, p := range peers {
				sendCh <- FormatMessage("PEER",
					"id", p.ID,
					"ip", p.IP,
					"port", p.PortStr(),
					"cpu", p.CPU,
					"ram_mb", p.RamStr(),
					"engines", p.EnginesCSV(),
				)
			}
			sendCh <- FormatMessage("PEERS_END", "count", strconv.Itoa(len(peers)))

		case "INVITE":
			targetID := msg.Params["target"]
			target, ok := reg.Get(targetID)
			if !ok || target.State != "IDLE" {
				sendCh <- FormatMessage("INVITE_NO", "target", targetID, "reason", "unavailable")
				continue
			}

			target.SendCh <- FormatMessage("INVITE_REQ",
				"from", current.ID,
				"from_ip", current.IP,
				"from_port", current.PortStr(),
				"job", msg.Params["job"],
				"engine", msg.Params["engine"],
				"frames", msg.Params["frames"],
			)

		case "INVITE_OK":
			directorID := msg.Params["to"]
			reg.SetState(current.ID, "BUSY")
			if director, ok := reg.Get(directorID); ok {
				director.SendCh <- FormatMessage("INVITED",
					"target", current.ID,
					"ip", current.IP,
					"port", current.PortStr(),
					"job", msg.Params["job"],
				)
			}

		case "INVITE_NO":
			directorID := msg.Params["to"]
			if director, ok := reg.Get(directorID); ok {
				director.SendCh <- FormatMessage("INVITE_NO",
					"target", current.ID,
					"job", msg.Params["job"],
					"reason", msg.Params["reason"],
				)
			}

		default:
			sendCh <- FormatMessage("ERR", "code", "unknown_verb", "verb", msg.Verb)
		}
	}
}

func newSessionID() string {
	b := make([]byte, 4)
	_, _ = rand.Read(b)
	return "s_" + hex.EncodeToString(b)
}
