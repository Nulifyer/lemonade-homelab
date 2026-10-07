package main

import (
	"context"
	"errors"
	"log/slog"
	"net"
	"net/http"
	"strconv"
	"time"
)

type requestAudit struct {
	id, model string
}

type auditKey struct{}

type auditWriter struct {
	http.ResponseWriter
	status int
	bytes  int64
}

func (w *auditWriter) WriteHeader(status int) {
	if status >= 100 && status < 200 {
		w.ResponseWriter.WriteHeader(status)
		return
	}
	if w.status != 0 {
		return
	}
	w.status = status
	w.ResponseWriter.WriteHeader(status)
}

func (w *auditWriter) Write(b []byte) (int, error) {
	if w.status == 0 {
		w.WriteHeader(http.StatusOK)
	}
	n, err := w.ResponseWriter.Write(b)
	w.bytes += int64(n)
	return n, err
}

func (w *auditWriter) Unwrap() http.ResponseWriter { return w.ResponseWriter }

func (w *auditWriter) Flush() {
	if w.status == 0 {
		w.WriteHeader(http.StatusOK)
	}
	_ = http.NewResponseController(w.ResponseWriter).Flush()
}

func (s *service) logEvent(level slog.Level, event string, attrs ...any) {
	if s.cfg.logger != nil {
		s.cfg.logger.Log(context.Background(), level, event, attrs...)
	}
}

func (s *service) newAudit() *requestAudit {
	return &requestAudit{id: "r" + strconv.FormatUint(s.sequence.Add(1), 10)}
}

func audit(r *http.Request) *requestAudit {
	value, _ := r.Context().Value(auditKey{}).(*requestAudit)
	return value
}

func logPath(path string) string {
	switch path {
	case "/", "/openapi.json", "/health", "/ready", "/metrics", "/v1/models", "/api/tags", "/api/version", "/v1/chat/completions", "/v1/completions", "/v1/responses", "/v1/audio/speech", "/v1/audio/transcriptions", "/api/chat", "/api/show":
		return path
	default:
		return "unrecognized"
	}
}

func errorKind(err error) string {
	if errors.Is(err, context.Canceled) {
		return "cancelled"
	}
	if errors.Is(err, context.DeadlineExceeded) {
		return "timeout"
	}
	var networkError net.Error
	if errors.As(err, &networkError) {
		if networkError.Timeout() {
			return "timeout"
		}
		return "connection"
	}
	return "backend"
}

func (s *service) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	started := time.Now()
	state := s.newAudit()
	path := logPath(r.URL.Path)
	r = r.WithContext(context.WithValue(r.Context(), auditKey{}, state))
	w.Header().Set("X-Request-ID", state.id)
	writer := &auditWriter{ResponseWriter: w}
	defer func() {
		status := writer.status
		if status == 0 {
			status = http.StatusOK
		}
		if path == "/health" && status == 200 {
			return
		}
		level := slog.LevelInfo
		if status >= 400 {
			level = slog.LevelWarn
		}
		s.logEvent(level, "http_request", "request_id", state.id, "method", r.Method, "path", path, "model", state.model, "status", status, "duration_ms", time.Since(started).Milliseconds(), "response_bytes", writer.bytes)
	}()
	s.handleHTTP(writer, r)
}
