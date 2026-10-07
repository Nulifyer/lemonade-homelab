package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"strings"
	"time"
)

var criticalModels = []struct{ role, name string }{
	{"small-task", "HA-Qwen35-2B"},
	{"speech-stt", "Redux-English"},
	{"speech-tts", "kokoro-v1"},
}

type modelResidency struct {
	Name          string `json:"model_name"`
	Loaded        bool   `json:"loaded"`
	Alive         bool   `json:"backend_alive"`
	BackendHealth string `json:"backend_health"`
	Pinned        bool   `json:"pinned"`
}

func (s *service) managerRequest(ctx context.Context, method, path string, body []byte, result any) error {
	q, err := http.NewRequestWithContext(ctx, method, strings.TrimRight(s.cfg.upstream.String(), "/")+path, bytes.NewReader(body))
	if err != nil {
		return err
	}
	if body != nil {
		q.Header.Set("Content-Type", "application/json")
	}
	if s.cfg.backendKey != "" {
		q.Header.Set("Authorization", "Bearer "+s.cfg.backendKey)
	}
	resp, err := s.client.Do(q)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return errors.New("manager returned non-success status")
	}
	if result == nil {
		_, err = io.Copy(io.Discard, io.LimitReader(resp.Body, 65536))
		return err
	}
	return json.NewDecoder(io.LimitReader(resp.Body, 1024*1024)).Decode(result)
}

func (s *service) criticalState(ctx context.Context) (map[string]bool, error) {
	ctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	var health struct {
		Status string           `json:"status"`
		Models []modelResidency `json:"all_models_loaded"`
	}
	if err := s.managerRequest(ctx, "GET", "/api/v1/health", nil, &health); err != nil {
		return nil, err
	}
	if health.Status != "ok" || health.Models == nil {
		return nil, errors.New("invalid manager health")
	}
	ready := map[string]bool{}
	for _, critical := range criticalModels {
		ready[critical.role] = false
		for _, model := range health.Models {
			if model.Name == critical.name {
				// Busy means a healthy backend is serving, not that it needs recovery.
				healthy := model.BackendHealth == "ready" || model.BackendHealth == "busy"
				ready[critical.role] = model.Loaded && model.Alive && healthy && model.Pinned
			}
		}
	}
	return ready, nil
}

func (s *service) serveCriticalReadiness(w http.ResponseWriter, r *http.Request) {
	ready, err := s.criticalState(r.Context())
	status := http.StatusOK
	if err != nil {
		status = http.StatusServiceUnavailable
	}
	for _, loaded := range ready {
		if !loaded {
			status = http.StatusServiceUnavailable
		}
	}
	writeJSON(w, status, map[string]any{"backend": err == nil, "critical_models": ready})
}

func (s *service) reconcileCriticalModels(ctx context.Context) error {
	ready, err := s.criticalState(ctx)
	if err != nil {
		return err
	}
	var failures []error
	for _, model := range criticalModels {
		if ready[model.role] {
			continue
		}
		if ctx.Err() != nil {
			return ctx.Err()
		}
		loadCtx, cancel := context.WithTimeout(ctx, 60*time.Second)
		var info struct {
			Downloaded bool `json:"downloaded"`
		}
		err := s.managerRequest(loadCtx, "GET", "/api/v1/models/"+model.name, nil, &info)
		if err == nil && !info.Downloaded {
			err = errors.New("critical model is not downloaded")
		}
		if err == nil {
			body, _ := json.Marshal(map[string]any{"model_name": model.name, "pinned": true})
			s.logEvent(slog.LevelInfo, "critical_model_loading", "model", model.role)
			err = s.managerRequest(loadCtx, "POST", "/api/v1/load", body, nil)
		}
		cancel()
		if err != nil {
			failures = append(failures, err)
			s.logEvent(slog.LevelWarn, "critical_model_failed", "model", model.role, "reason", errorKind(err))
		} else {
			s.logEvent(slog.LevelInfo, "critical_model_load_completed", "model", model.role)
		}
	}
	return errors.Join(failures...)
}

func (s *service) warmupLoop(ctx context.Context, interval time.Duration) {
	for {
		if err := s.reconcileCriticalModels(ctx); err != nil && ctx.Err() == nil {
			s.logEvent(slog.LevelWarn, "warmup_retry", "reason", errorKind(err))
		}
		timer := time.NewTimer(interval)
		select {
		case <-ctx.Done():
			timer.Stop()
			return
		case <-timer.C:
		}
	}
}

func (s *service) runWarmup(ctx context.Context) {
	workerDone := make(chan struct{})
	go func() {
		defer close(workerDone)
		s.warmupLoop(ctx, 15*time.Second)
	}()
	server := &http.Server{Addr: s.cfg.httpAddr, ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 10 * time.Second, IdleTimeout: 30 * time.Second, Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == "GET" && r.URL.Path == "/health" {
			writeJSON(w, 200, map[string]bool{"alive": true})
		} else if r.Method == "GET" && r.URL.Path == "/ready" {
			s.serveCriticalReadiness(w, r)
		} else {
			http.NotFound(w, r)
		}
	})}
	go func() {
		<-ctx.Done()
		shutdown, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = server.Shutdown(shutdown)
	}()
	s.logEvent(slog.LevelInfo, "warmup_started", "models", []string{"small-task", "speech-stt", "speech-tts"})
	if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		panic(err)
	}
	<-workerDone
}
