package main

import (
	"bytes"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestCatalogMatchesConsumerScope(t *testing.T) {
	s := testService(t, func(http.ResponseWriter, *http.Request) { t.Fatal("catalog reached backend") })
	s.cfg.publicURL = "https://ai.example.test"
	s.cfg.chatURL = "https://chat.example.test"
	for _, noAuth := range []bool{false, true} {
		s.cfg.noAuth = noAuth
		for _, path := range []string{"/", "/openapi.json"} {
			r := httptest.NewRequest("GET", path, nil)
			if !noAuth {
				r.Header.Set("Authorization", "Bearer "+strings.Repeat("h", 32))
			}
			w := httptest.NewRecorder()
			s.ServeHTTP(w, r)
			if w.Code != 200 {
				t.Fatal(w.Code, w.Body.String())
			}
			body := w.Body.String()
			if !strings.Contains(body, "small-task") || strings.Contains(body, "agent-work") != noAuth {
				t.Fatal("wrong catalog scope", body)
			}
			if path == "/openapi.json" {
				var doc struct {
					OpenAPI  string
					Servers  []struct{ URL string }
					Paths    map[string]any
					Security []any
				}
				if err := json.Unmarshal(w.Body.Bytes(), &doc); err != nil {
					t.Fatal(err)
				}
				if doc.OpenAPI != "3.0.3" || doc.Servers[0].URL != s.cfg.publicURL || (len(doc.Security) == 0) != noAuth {
					t.Fatal("wrong API metadata", body)
				}
				_, speech := doc.Paths["/v1/audio/speech"]
				if speech != noAuth {
					t.Fatal("speech path outside scope")
				}
				for path := range doc.Paths {
					if strings.Contains(path, "internal") || strings.Contains(path, "pull") {
						t.Fatal("admin path in inference contract")
					}
				}
			}
		}
	}
}

func TestRequestLogsExcludeClientContent(t *testing.T) {
	s := testService(t, func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, map[string]string{"text": "private-output"})
	})
	var logs bytes.Buffer
	s.cfg.logger = slog.New(slog.NewJSONHandler(&logs, nil))
	r := httptest.NewRequest("POST", "/v1/chat/completions", strings.NewReader(`{"model":"small-task","messages":[{"role":"user","content":"private-prompt"}]}`))
	r.Header.Set("Content-Type", "application/json")
	r.Header.Set("Authorization", "Bearer "+strings.Repeat("h", 32))
	w := httptest.NewRecorder()
	s.ServeHTTP(w, r)
	if w.Header().Get("X-Request-ID") == "" {
		t.Fatal("missing request ID")
	}
	q := httptest.NewRequest("GET", "/private-path?secret=private-query", nil)
	q.Header.Set("Authorization", "Bearer "+strings.Repeat("h", 32))
	s.ServeHTTP(httptest.NewRecorder(), q)
	for _, forbidden := range []string{"private-prompt", "private-output", "private-path", "private-query", strings.Repeat("h", 32), strings.Repeat("b", 32)} {
		if strings.Contains(logs.String(), forbidden) {
			t.Fatal("private content logged", forbidden)
		}
	}
	scanner := json.NewDecoder(&logs)
	seen := 0
	for {
		var row map[string]any
		err := scanner.Decode(&row)
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		if row["msg"] == "http_request" {
			seen++
			if row["request_id"] == "" || row["duration_ms"] == nil || row["status"] == nil {
				t.Fatal("missing audit metadata", row)
			}
		}
	}
	if seen != 2 {
		t.Fatal("missing completed request logs", seen)
	}
}

func TestAuditWriterPreservesStreaming(t *testing.T) {
	flushed := make(chan struct{})
	release := make(chan struct{})
	backend := func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		io.WriteString(w, "data: first\n\n")
		w.(http.Flusher).Flush()
		close(flushed)
		<-release
		io.WriteString(w, "data: [DONE]\n\n")
	}
	s := testService(t, backend)
	s.cfg.noAuth = true
	server := httptest.NewServer(s)
	defer server.Close()
	req, _ := http.NewRequest("POST", server.URL+"/v1/chat/completions", strings.NewReader(`{"model":"small-task","stream":true}`))
	req.Header.Set("Content-Type", "application/json")
	resp, err := server.Client().Do(req)
	if err != nil {
		close(release)
		t.Fatal(err)
	}
	defer resp.Body.Close()
	<-flushed
	first := make([]byte, len("data: first\n\n"))
	if _, err := io.ReadFull(resp.Body, first); err != nil {
		close(release)
		t.Fatal(err)
	}
	close(release)
	rest, err := io.ReadAll(resp.Body)
	if err != nil || string(first) != "data: first\n\n" || string(rest) != "data: [DONE]\n\n" {
		t.Fatal("stream altered", string(first), string(rest), err)
	}
}

func TestSpeechAuditExcludesText(t *testing.T) {
	s := testService(t, func(w http.ResponseWriter, r *http.Request) {
		io.Copy(io.Discard, r.Body)
		w.Write([]byte("private-audio"))
	})
	var logs bytes.Buffer
	s.cfg.logger = slog.New(slog.NewJSONHandler(&logs, nil))
	_, err := s.synthesize(t.Context(), "private-utterance", "af_heart")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(logs.String(), "speech_request") || !strings.Contains(logs.String(), "speech-tts") {
		t.Fatal("speech metadata absent")
	}
	if strings.Contains(logs.String(), "private-utterance") || strings.Contains(logs.String(), "private-audio") {
		t.Fatal("speech content logged")
	}
}
