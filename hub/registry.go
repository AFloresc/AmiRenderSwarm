package main

import (
	"net"
	"strconv"
	"strings"
	"sync"
	"time"
)

type Agent struct {
	ID         string
	SessionID  string
	OS         string
	CPU        string
	FPU        bool
	RamMB      int
	Engines    []string
	Role       string // worker | director | both
	IP         string
	ListenPort int
	State      string // IDLE | BUSY
	Load       string
	LastSeen   time.Time
	Conn       net.Conn
	SendCh     chan string
}

func (a *Agent) HasEngine(engine string) bool {
	if engine == "" {
		return true
	}
	target := strings.ToLower(engine)
	for _, e := range a.Engines {
		if strings.ToLower(e) == target {
			return true
		}
	}
	return false
}

type Registry struct {
	mu     sync.RWMutex
	agents map[string]*Agent
}

func NewRegistry() *Registry {
	return &Registry{
		agents: make(map[string]*Agent),
	}
}

func (r *Registry) Register(a *Agent) {
	r.mu.Lock()
	defer r.mu.Unlock()

	if old, exists := r.agents[a.ID]; exists && old.Conn != nil {
		_ = old.Conn.Close()
	}
	r.agents[a.ID] = a
}

func (r *Registry) Unregister(id, sessionID string) {
	r.mu.Lock()
	defer r.mu.Unlock()

	if cur, exists := r.agents[id]; exists && cur.SessionID == sessionID {
		delete(r.agents, id)
	}
}

func (r *Registry) Get(id string) (*Agent, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	a, ok := r.agents[id]
	return a, ok
}

func (r *Registry) UpdateHeartbeat(id, state, load string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()

	a, ok := r.agents[id]
	if !ok {
		return false
	}
	a.LastSeen = time.Now()
	if state != "" {
		a.State = strings.ToUpper(state)
	}
	if load != "" {
		a.Load = load
	}
	return true
}

func (r *Registry) SetState(id, state string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()

	a, ok := r.agents[id]
	if !ok {
		return false
	}
	a.State = strings.ToUpper(state)
	a.LastSeen = time.Now()
	return true
}

func (r *Registry) ListAvailable(requesterID, engine string, minRAM int) []*Agent {
	r.mu.RLock()
	defer r.mu.RUnlock()

	var list []*Agent
	for _, a := range r.agents {
		if a.ID == requesterID {
			continue
		}
		if a.State != "IDLE" {
			continue
		}
		if a.Role != "worker" && a.Role != "both" {
			continue
		}
		if a.RamMB < minRAM {
			continue
		}
		if !a.HasEngine(engine) {
			continue
		}
		list = append(list, a)
	}
	return list
}

func (r *Registry) SweepStale(timeout time.Duration) []string {
	r.mu.Lock()
	defer r.mu.Unlock()

	now := time.Now()
	var evicted []string
	for id, a := range r.agents {
		if now.Sub(a.LastSeen) > timeout {
			if a.Conn != nil {
				_ = a.Conn.Close()
			}
			delete(r.agents, id)
			evicted = append(evicted, id)
		}
	}
	return evicted
}

func (a *Agent) EnginesCSV() string {
	return strings.Join(a.Engines, ",")
}

func (a *Agent) PortStr() string {
	return strconv.Itoa(a.ListenPort)
}

func (a *Agent) RamStr() string {
	return strconv.Itoa(a.RamMB)
}
