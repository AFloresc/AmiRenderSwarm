package main

import (
	"fmt"
	"strings"
)

// Message representa una línea del protocolo: VERB key=val key=val
type Message struct {
	Verb   string
	Params map[string]string
}

// ParseMessage convierte una línea de texto en un Message estructurado.
func ParseMessage(line string) (Message, error) {
	line = strings.TrimSpace(line)
	if line == "" {
		return Message{}, fmt.Errorf("empty line")
	}

	parts := strings.Fields(line)
	msg := Message{
		Verb:   strings.ToUpper(parts[0]),
		Params: make(map[string]string, len(parts)-1),
	}

	for _, token := range parts[1:] {
		idx := strings.IndexByte(token, '=')
		if idx <= 0 {
			continue
		}
		key := strings.ToLower(token[:idx])
		val := token[idx+1:]
		msg.Params[key] = val
	}

	return msg, nil
}

// FormatMessage construye una línea lista para enviar por TCP.
func FormatMessage(verb string, kv ...string) string {
	var b strings.Builder
	b.WriteString(strings.ToUpper(verb))
	for i := 0; i+1 < len(kv); i += 2 {
		if kv[i+1] == "" {
			continue
		}
		b.WriteByte(' ')
		b.WriteString(kv[i])
		b.WriteByte('=')
		b.WriteString(kv[i+1])
	}
	b.WriteByte('\n')
	return b.String()
}
