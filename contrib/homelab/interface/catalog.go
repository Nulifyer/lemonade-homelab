package main

import (
	_ "embed"
	"encoding/json"
	"html/template"
	"net/http"
)

type modelRole struct {
	ID      string
	Purpose string
	Path    string
}

var modelRoles = []modelRole{
	{"small-task", "Home Assistant and small general tasks", "/v1/chat/completions"},
	{"chat-roleplay", "Roleplay and conversation", "/v1/chat/completions"},
	{"agent-work", "Research, coding and general tasks", "/v1/chat/completions"},
	{"speech-stt", "English speech to text, WAV upload", "/v1/audio/transcriptions"},
	{"speech-tts", "English utility speech", "/v1/audio/speech"},
}

//go:embed index.html
var indexHTML string

//go:embed openapi.json
var openAPIJSON []byte

var servicePage = template.Must(template.New("service").Parse(indexHTML))

func availableRoles(models map[string]bool) []modelRole {
	roles := []modelRole{}
	for _, role := range modelRoles {
		if models[role.ID] {
			roles = append(roles, role)
		}
	}
	return roles
}

func (s *service) serveCatalog(w http.ResponseWriter, r *http.Request, models map[string]bool) bool {
	if r.Method != http.MethodGet {
		return false
	}
	switch r.URL.Path {
	case "/":
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		_ = servicePage.Execute(w, struct {
			PublicURL, ChatURL, ManagerURL, ConsoleURL string
			NoAuth                                     bool
			Roles                                      []modelRole
		}{s.cfg.publicURL, s.cfg.chatURL, s.cfg.managerURL, s.cfg.consoleURL, s.cfg.noAuth, availableRoles(models)})
		return true
	case "/openapi.json":
		var doc map[string]any
		if err := json.Unmarshal(openAPIJSON, &doc); err != nil {
			http.Error(w, "service contract unavailable", 500)
			return true
		}
		doc["servers"] = []map[string]string{{"url": s.cfg.publicURL}}
		if !s.cfg.noAuth {
			doc["security"] = []map[string][]string{{"bearerAuth": {}}}
		}
		paths := doc["paths"].(map[string]any)
		for path, raw := range paths {
			operation, exists := raw.(map[string]any)["post"]
			if !exists {
				continue
			}
			body := operation.(map[string]any)["requestBody"].(map[string]any)["content"].(map[string]any)
			media := "application/json"
			if path == "/v1/audio/transcriptions" {
				media = "multipart/form-data"
			}
			schema := body[media].(map[string]any)["schema"].(map[string]any)["properties"].(map[string]any)["model"].(map[string]any)
			names := []string{}
			for _, role := range availableRoles(models) {
				if role.Path == path || (role.Path == "/v1/chat/completions" && (path == "/api/chat" || path == "/api/show" || path == "/v1/completions" || path == "/v1/responses")) {
					name := role.ID
					if path == "/api/chat" || path == "/api/show" {
						name += ":latest"
					}
					names = append(names, name)
				}
			}
			if len(names) == 0 {
				delete(paths, path)
			} else {
				schema["enum"] = names
			}
		}
		writeJSON(w, 200, doc)
		return true
	}
	return false
}
