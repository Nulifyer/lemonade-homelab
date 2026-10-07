package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestCriticalModelsRetryAndManagerRestart(t *testing.T) {
	var mu sync.Mutex
	loaded := map[string]modelResidency{}
	loads := map[string]int{}
	unavailable := true
	failSTT := true
	s := testService(t, func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		defer mu.Unlock()
		if unavailable {
			http.Error(w, "offline", 503)
			return
		}
		switch {
		case r.URL.Path == "/api/v1/health":
			list := []modelResidency{}
			for _, model := range loaded {
				list = append(list, model)
			}
			writeJSON(w, 200, map[string]any{"status": "ok", "all_models_loaded": list})
		case r.Method == "GET" && strings.HasPrefix(r.URL.Path, "/api/v1/models/"):
			writeJSON(w, 200, map[string]bool{"downloaded": true})
		case r.URL.Path == "/api/v1/load":
			var body struct {
				Name   string `json:"model_name"`
				Pinned bool   `json:"pinned"`
			}
			if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
				t.Error(err)
			}
			if !body.Pinned {
				t.Error("critical load is not pinned")
			}
			if body.Name != "HA-Qwen35-2B" && body.Name != "Redux-English" && body.Name != "kokoro-v1" {
				t.Error("noncritical model loaded", body.Name)
			}
			loads[body.Name]++
			if failSTT && body.Name == "Redux-English" {
				http.Error(w, "failed", 500)
				return
			}
			loaded[body.Name] = modelResidency{Name: body.Name, Loaded: true, Alive: true, BackendHealth: "ready", Pinned: true}
			writeJSON(w, 200, map[string]bool{"ok": true})
		default:
			t.Error("unexpected endpoint", r.URL.Path)
			http.NotFound(w, r)
		}
	})
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { defer close(done); s.warmupLoop(ctx, 10*time.Millisecond) }()
	defer func() { cancel(); <-done }()
	waitFor := func(check func() bool) {
		t.Helper()
		deadline := time.Now().Add(2 * time.Second)
		for time.Now().Before(deadline) {
			mu.Lock()
			ok := check()
			mu.Unlock()
			if ok {
				return
			}
			time.Sleep(5 * time.Millisecond)
		}
		t.Fatal("warmup did not converge")
	}
	w := httptest.NewRecorder()
	s.serveCriticalReadiness(w, httptest.NewRequest("GET", "/ready", nil))
	if w.Code != 503 {
		t.Fatal("offline manager was ready", w.Code)
	}
	mu.Lock()
	unavailable = false
	mu.Unlock()
	waitFor(func() bool { return len(loaded) == 2 && loads["Redux-English"] > 0 })
	w = httptest.NewRecorder()
	s.serveCriticalReadiness(w, httptest.NewRequest("GET", "/ready", nil))
	if w.Code != 503 {
		t.Fatal("partial models were ready", w.Code)
	}
	mu.Lock()
	failSTT = false
	mu.Unlock()
	waitFor(func() bool { return len(loaded) == 3 })
	w = httptest.NewRecorder()
	s.serveCriticalReadiness(w, httptest.NewRequest("GET", "/ready", nil))
	if w.Code != 200 {
		t.Fatal("ready models rejected", w.Body.String())
	}
	mu.Lock()
	baseline := loads["HA-Qwen35-2B"]
	mu.Unlock()
	time.Sleep(40 * time.Millisecond)
	mu.Lock()
	if loads["HA-Qwen35-2B"] != baseline {
		t.Error("healthy resident model reloaded")
	}
	loaded = map[string]modelResidency{}
	unavailable = true
	mu.Unlock()
	w = httptest.NewRecorder()
	s.serveCriticalReadiness(w, httptest.NewRequest("GET", "/ready", nil))
	if w.Code != 503 {
		t.Fatal("stale readiness after manager restart")
	}
	mu.Lock()
	unavailable = false
	mu.Unlock()
	waitFor(func() bool { return len(loaded) == 3 && loads["HA-Qwen35-2B"] > baseline })
}

func TestCriticalReadinessRequiresHealthyPinnedBackends(t *testing.T) {
	for _, field := range []string{"loaded", "backend_alive", "backend_health", "pinned"} {
		t.Run(field, func(t *testing.T) {
			s := testService(t, func(w http.ResponseWriter, r *http.Request) {
				list := []map[string]any{}
				for _, model := range criticalModels {
					item := map[string]any{"model_name": model.name, "loaded": true, "backend_alive": true, "backend_health": "ready", "pinned": true}
					if model.role == "small-task" {
						if field == "backend_health" {
							item[field] = "failed"
						} else {
							item[field] = false
						}
					}
					list = append(list, item)
				}
				writeJSON(w, 200, map[string]any{"status": "ok", "all_models_loaded": list})
			})
			w := httptest.NewRecorder()
			s.serveCriticalReadiness(w, httptest.NewRequest("GET", "/ready", nil))
			if w.Code != 503 {
				t.Fatal("false positive readiness", field, w.Body.String())
			}
		})
	}
}

func TestBusyCriticalModelRemainsReadyWithoutReload(t *testing.T) {
	for _, busy := range criticalModels {
		t.Run(busy.role, func(t *testing.T) {
			s := testService(t, func(w http.ResponseWriter, r *http.Request) {
				if r.Method != "GET" || r.URL.Path != "/api/v1/health" {
					t.Errorf("busy resident model triggered lifecycle request: %s %s", r.Method, r.URL.Path)
					http.Error(w, "unexpected reload", 500)
					return
				}
				list := []modelResidency{}
				for _, model := range criticalModels {
					health := "ready"
					if model.role == busy.role {
						health = "busy"
					}
					list = append(list, modelResidency{Name: model.name, Loaded: true, Alive: true, BackendHealth: health, Pinned: true})
				}
				writeJSON(w, 200, map[string]any{"status": "ok", "all_models_loaded": list})
			})
			w := httptest.NewRecorder()
			s.serveCriticalReadiness(w, httptest.NewRequest("GET", "/ready", nil))
			if w.Code != 200 {
				t.Errorf("serving model was unavailable: %s", w.Body.String())
			}
			if err := s.reconcileCriticalModels(context.Background()); err != nil {
				t.Fatal("busy resident model was reconciled", err)
			}
		})
	}
}

func TestWarmupDoesNotDownloadMissingModels(t *testing.T) {
	loads := 0
	s := testService(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/api/v1/health":
			writeJSON(w, 200, map[string]any{"status": "ok", "all_models_loaded": []any{}})
		case "/api/v1/load":
			loads++
			writeJSON(w, 200, map[string]bool{"ok": true})
		default:
			writeJSON(w, 200, map[string]bool{"downloaded": false})
		}
	})
	if err := s.reconcileCriticalModels(context.Background()); err == nil {
		t.Fatal("missing models accepted")
	}
	if loads != 0 {
		t.Fatal("missing model load would trigger a download")
	}
}

func TestWarmupShutdownCancelsLoad(t *testing.T) {
	started := make(chan struct{})
	canceled := make(chan struct{})
	s := testService(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/api/v1/health":
			writeJSON(w, 200, map[string]any{"status": "ok", "all_models_loaded": []any{}})
		case "/api/v1/load":
			_, _ = json.NewDecoder(r.Body).Token()
			close(started)
			<-r.Context().Done()
			close(canceled)
		default:
			writeJSON(w, 200, map[string]bool{"downloaded": true})
		}
	})
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { defer close(done); s.warmupLoop(ctx, time.Hour) }()
	select {
	case <-started:
	case <-time.After(time.Second):
		t.Fatal("load did not start")
	}
	cancel()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("warmup did not stop")
	}
	select {
	case <-canceled:
	case <-time.After(time.Second):
		t.Fatal("load not canceled")
	}
}
