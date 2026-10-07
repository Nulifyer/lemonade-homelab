package main

import (
	"context"
	"crypto/subtle"
	"encoding/json"
	"errors"
	"io"
	"log"
	"log/slog"
	"mime"
	"mime/multipart"
	"net"
	"net/http"
	"net/http/httputil"
	"net/url"
	"os"
	"os/signal"
	"strings"
	"sync/atomic"
	"syscall"
	"time"
)

type config struct {
	upstream                                   *url.URL
	backendKey                                 string
	noAuth                                     bool
	logger                                     *slog.Logger
	publicURL, chatURL, managerURL, consoleURL string
	clients                                    map[string]map[string]bool
	voice                                      string
	httpAddr, voiceAddr                        string
	inferenceTimeout                           time.Duration
}

type service struct {
	cfg                config
	client             *http.Client
	proxy              *httputil.ReverseProxy
	stt, tts           chan struct{}
	admitted           chan struct{}
	voiceClients       chan struct{}
	requests, failures atomic.Uint64
	sequence           atomic.Uint64
}

func readConfig() (config, error) {
	u, err := url.Parse(os.Getenv("LEMONADE_URL"))
	if err != nil || u == nil || u.Host == "" || (u.Scheme != "http" && u.Scheme != "https") || u.User != nil {
		return config{}, errors.New("LEMONADE_URL must be an HTTP backend URL")
	}
	c := config{upstream: u, backendKey: os.Getenv("LEMONADE_BACKEND_KEY"), voice: "af_heart", httpAddr: ":8080", voiceAddr: ":10300", inferenceTimeout: 20 * time.Minute, clients: map[string]map[string]bool{}}
	if v := os.Getenv("INFERENCE_TIMEOUT"); v != "" {
		c.inferenceTimeout, err = time.ParseDuration(v)
		if err != nil || c.inferenceTimeout < 15*time.Second || c.inferenceTimeout > 30*time.Minute {
			return c, errors.New("INFERENCE_TIMEOUT must be between 15s and 30m")
		}
	}
	switch os.Getenv("AUTH_MODE") {
	case "", "keys":
	case "none":
		c.noAuth = true
	default:
		return c, errors.New("AUTH_MODE must be keys or none")
	}
	if !c.noAuth {
		for _, p := range []struct {
			env    string
			models []string
		}{{"HA_API_KEY", []string{"small-task"}}, {"LUNCHLOXS_API_KEY", []string{"agent-work", "small-task"}}, {"ROLEPLAY_API_KEY", []string{"chat-roleplay"}}, {"GENERAL_API_KEY", []string{"small-task", "agent-work", "chat-roleplay", "speech-stt", "speech-tts"}}} {
			key := os.Getenv(p.env)
			if len(key) < 32 {
				return c, errors.New(p.env + " must contain at least 32 characters")
			}
			if _, ok := c.clients[key]; ok {
				return c, errors.New("consumer keys must be distinct")
			}
			roles := map[string]bool{}
			for _, m := range p.models {
				roles[m] = true
			}
			c.clients[key] = roles
		}
		if len(c.backendKey) < 32 {
			return c, errors.New("LEMONADE_BACKEND_KEY is required")
		}
	}
	if v := os.Getenv("TTS_VOICE"); v != "" {
		c.voice = v
	}
	if v := os.Getenv("HTTP_ADDR"); v != "" {
		c.httpAddr = v
	}
	if v := os.Getenv("WYOMING_ADDR"); v != "" {
		c.voiceAddr = v
	}
	c.publicURL = "http://localhost:8080"
	for _, item := range []struct {
		name string
		dest *string
	}{{"AI_PUBLIC_URL", &c.publicURL}, {"CHAT_URL", &c.chatURL}, {"MANAGER_URL", &c.managerURL}, {"CONSOLE_URL", &c.consoleURL}} {
		if value := os.Getenv(item.name); value != "" {
			u, err := url.Parse(value)
			if err != nil || u.Host == "" || u.User != nil || (u.Scheme != "http" && u.Scheme != "https") || u.RawQuery != "" || u.Fragment != "" {
				return c, errors.New(item.name + " must be an HTTP service URL without credentials, query or fragment")
			}
			*item.dest = strings.TrimRight(value, "/")
		}
	}
	return c, nil
}

func newService(c config) *service {
	if c.inferenceTimeout <= 0 {
		c.inferenceTimeout = 20 * time.Minute
	}
	transport := &http.Transport{Proxy: nil, DialContext: (&net.Dialer{Timeout: 5 * time.Second, KeepAlive: 30 * time.Second}).DialContext, MaxIdleConns: 16, MaxIdleConnsPerHost: 8, IdleConnTimeout: 30 * time.Second, ResponseHeaderTimeout: c.inferenceTimeout}
	s := &service{cfg: c, client: &http.Client{Transport: transport, Timeout: 70 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}, stt: make(chan struct{}, 1), tts: make(chan struct{}, 1), admitted: make(chan struct{}, 8), voiceClients: make(chan struct{}, 16)}
	s.proxy = &httputil.ReverseProxy{Transport: transport, FlushInterval: -1, Rewrite: func(pr *httputil.ProxyRequest) {
		pr.SetURL(c.upstream)
		pr.Out.Header.Del("Authorization")
		if c.backendKey != "" {
			pr.Out.Header.Set("Authorization", "Bearer "+c.backendKey)
		}
		pr.Out.Header.Del("Cookie")
		pr.Out.Header.Del("Proxy-Authorization")
		pr.Out.Header.Del("X-Api-Key")
	}, ErrorHandler: func(w http.ResponseWriter, r *http.Request, err error) {
		s.failures.Add(1)
		if state := audit(r); state != nil {
			s.logEvent(slog.LevelWarn, "backend_failure", "request_id", state.id, "model", state.model, "reason", errorKind(err))
		}
		http.Error(w, "model backend unavailable; reconnect with a fresh request", http.StatusBadGateway)
	}, ErrorLog: log.New(io.Discard, "", 0)}
	return s
}

func (s *service) authorized(r *http.Request) map[string]bool {
	if s.cfg.noAuth {
		models := map[string]bool{}
		for _, role := range modelRoles {
			models[role.ID] = true
		}
		return models
	}
	value := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
	for key, models := range s.cfg.clients {
		if subtle.ConstantTimeCompare([]byte(key), []byte(value)) == 1 {
			return models
		}
	}
	return nil
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func (s *service) handleHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method == "GET" && r.URL.Path == "/health" {
		writeJSON(w, 200, map[string]bool{"alive": true})
		return
	}
	models := s.authorized(r)
	if models == nil {
		http.Error(w, "unauthorized", 401)
		return
	}
	if s.serveCatalog(w, r, models) {
		return
	}
	if r.Method == "GET" && r.URL.Path == "/ready" {
		ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
		defer cancel()
		q, _ := http.NewRequestWithContext(ctx, "GET", strings.TrimSuffix(s.cfg.upstream.String(), "/")+"/live", nil)
		resp, err := s.client.Do(q)
		if err != nil {
			http.Error(w, "backend unavailable", 503)
			return
		}
		defer resp.Body.Close()
		if resp.StatusCode != 200 {
			http.Error(w, "backend unavailable", 503)
			return
		}
		writeJSON(w, 200, map[string]bool{"backend": true})
		return
	}
	if r.Method == "GET" && (r.URL.Path == "/v1/models" || r.URL.Path == "/api/tags") {
		list := []map[string]any{}
		for _, role := range modelRoles {
			name := role.ID
			if !models[name] {
				continue
			}
			if r.URL.Path == "/api/tags" {
				list = append(list, map[string]any{"name": name + ":latest", "model": name + ":latest", "modified_at": "2026-10-07T00:00:00Z", "size": 0, "digest": name})
			} else {
				list = append(list, map[string]any{"id": name, "object": "model", "owned_by": "homelab"})
			}
		}
		if r.URL.Path == "/api/tags" {
			writeJSON(w, 200, map[string]any{"models": list})
		} else {
			writeJSON(w, 200, map[string]any{"object": "list", "data": list})
		}
		return
	}
	if r.Method == "GET" && r.URL.Path == "/api/version" {
		writeJSON(w, 200, map[string]string{"version": "0.16.1"})
		return
	}
	if r.Method == "GET" && r.URL.Path == "/metrics" {
		w.Header().Set("Content-Type", "text/plain; version=0.0.4")
		_, _ = io.WriteString(w, "# TYPE homelab_ai_requests_total counter\nhomelab_ai_requests_total "+uintString(s.requests.Load())+"\n# TYPE homelab_ai_failures_total counter\nhomelab_ai_failures_total "+uintString(s.failures.Load())+"\n")
		return
	}
	if r.Method == "POST" && r.URL.Path == "/v1/audio/transcriptions" {
		if !models["speech-stt"] || r.URL.RawQuery != "" || r.Header.Get("Content-Encoding") != "" {
			http.Error(w, "speech endpoint not allowed", 403)
			return
		}
		media, params, err := mime.ParseMediaType(r.Header.Get("Content-Type"))
		if err != nil || media != "multipart/form-data" || params["boundary"] == "" {
			http.Error(w, "multipart WAV required", 415)
			return
		}
		body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 16*1024*1024+65536))
		if err != nil {
			http.Error(w, "audio too large", 413)
			return
		}
		reader := multipart.NewReader(strings.NewReader(string(body)), params["boundary"])
		seen := map[string]bool{}
		validFile := false
		validModel := false
		for {
			part, err := reader.NextPart()
			if err == io.EOF {
				break
			}
			if err != nil {
				http.Error(w, "invalid multipart", 400)
				return
			}
			name := part.FormName()
			if seen[name] || name == "" {
				http.Error(w, "ambiguous multipart", 400)
				return
			}
			seen[name] = true
			data, err := io.ReadAll(part)
			if err != nil {
				http.Error(w, "invalid upload", 400)
				return
			}
			if name == "model" {
				validModel = string(data) == "speech-stt"
			}
			if name == "file" {
				validFile = len(data) >= 12 && string(data[:4]) == "RIFF" && string(data[8:12]) == "WAVE"
			}
		}
		if !validFile || !validModel {
			http.Error(w, "speech-stt WAV required", 400)
			return
		}
		if state := audit(r); state != nil {
			state.model = "speech-stt"
		}
		s.forward(w, r, body)
		return
	}
	allowed := r.Method == "POST" && (r.URL.Path == "/v1/chat/completions" || r.URL.Path == "/v1/completions" || r.URL.Path == "/v1/responses" || r.URL.Path == "/v1/audio/speech" || r.URL.Path == "/api/chat" || r.URL.Path == "/api/show")
	if !allowed || r.URL.RawQuery != "" {
		http.Error(w, "inference endpoint not allowed", 403)
		return
	}
	media, _, mediaErr := mime.ParseMediaType(r.Header.Get("Content-Type"))
	if r.Header.Get("Content-Encoding") != "" || mediaErr != nil || media != "application/json" {
		http.Error(w, "JSON required", 415)
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, 4*1024*1024)
	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, "request too large", 413)
		return
	}
	value, err := decodeUniqueJSON(body)
	if err != nil {
		http.Error(w, "invalid or ambiguous JSON", 400)
		return
	}
	obj, ok := value.(map[string]any)
	if !ok {
		http.Error(w, "JSON object required", 400)
		return
	}
	model, _ := obj["model"].(string)
	model = strings.TrimSuffix(model, ":latest")
	if !models[model] {
		http.Error(w, "model not allowed", 403)
		return
	}
	if strings.Contains(r.URL.Path, "audio/speech") && model != "speech-tts" {
		http.Error(w, "speech model required", 400)
		return
	}
	if !strings.Contains(r.URL.Path, "audio/speech") && strings.HasPrefix(model, "speech-") {
		http.Error(w, "LLM model required", 400)
		return
	}
	if state := audit(r); state != nil {
		state.model = model
	}
	if err := applyPreset(obj, model, r.URL.Path); err != nil {
		http.Error(w, err.Error(), 400)
		return
	}
	body, err = json.Marshal(obj)
	if err != nil {
		http.Error(w, "invalid request", 400)
		return
	}
	s.forward(w, r, body)
}

func (s *service) forward(w http.ResponseWriter, r *http.Request, body []byte) {
	select {
	case s.admitted <- struct{}{}:
		defer func() { <-s.admitted }()
	default:
		http.Error(w, "inference queue full", 429)
		return
	}
	if state := audit(r); state != nil {
		s.logEvent(slog.LevelInfo, "inference_started", "request_id", state.id, "model", state.model, "path", logPath(r.URL.Path))
	}
	ctx, cancel := context.WithTimeout(r.Context(), s.cfg.inferenceTimeout)
	defer cancel()
	r = r.WithContext(ctx)
	r.Body = io.NopCloser(strings.NewReader(string(body)))
	r.ContentLength = int64(len(body))
	r.GetBody = nil
	if strings.HasPrefix(r.URL.Path, "/v1/") {
		r.URL.Path = "/api" + r.URL.Path
	}
	s.requests.Add(1)
	s.proxy.ServeHTTP(w, r)
}

func main() {
	c, err := readConfig()
	if err != nil {
		log.Fatal(err)
	}
	c.logger = slog.New(slog.NewJSONHandler(os.Stderr, nil))
	s := newService(c)
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	listener, err := net.Listen("tcp", c.voiceAddr)
	if err != nil {
		log.Fatal(err)
	}
	go s.serveVoice(ctx, listener)
	server := &http.Server{Addr: c.httpAddr, Handler: s, ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 15 * time.Second, IdleTimeout: 60 * time.Second, MaxHeaderBytes: 16384}
	go func() {
		<-ctx.Done()
		listener.Close()
		shutdown, done := context.WithTimeout(context.Background(), 10*time.Second)
		defer done()
		_ = server.Shutdown(shutdown)
	}()
	s.logEvent(slog.LevelInfo, "service_started", "http_address", c.httpAddr, "wyoming_address", c.voiceAddr, "key_free", c.noAuth)
	if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatal(err)
	}
}
