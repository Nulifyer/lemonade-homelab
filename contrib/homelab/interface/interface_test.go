package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"io"
	"math"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"
)

func testService(t *testing.T, handler http.HandlerFunc) *service {
	t.Helper()
	backend := httptest.NewServer(handler)
	t.Cleanup(backend.Close)
	u, _ := url.Parse(backend.URL)
	return newService(config{upstream: u, backendKey: strings.Repeat("b", 32), voice: "af_heart", clients: map[string]map[string]bool{strings.Repeat("h", 32): {"small-task": true}, strings.Repeat("g", 32): {"small-task": true, "speech-stt": true, "speech-tts": true}}})
}
func TestInferenceAuthorization(t *testing.T) {
	calls := 0
	s := testService(t, func(w http.ResponseWriter, r *http.Request) {
		calls++
		if r.Header.Get("Authorization") != "Bearer "+strings.Repeat("b", 32) {
			t.Error("upstream key not replaced")
		}
		if r.Header.Get("Cookie") != "" {
			t.Error("cookie leaked")
		}
		if r.URL.Path != "/api/v1/chat/completions" {
			t.Error(r.URL.Path)
		}
		writeJSON(w, 200, map[string]any{"ok": true})
	})
	for _, tc := range []struct {
		path, body, key string
		status          int
	}{{"/v1/chat/completions", `{"model":"small-task"}`, "", 401}, {"/v1/chat/completions", `{"model":"agent-work"}`, strings.Repeat("h", 32), 403}, {"/v1/chat/completions", `{"model":"small-task","model":"agent-work"}`, strings.Repeat("h", 32), 400}, {"/v1/chat/completions", `{"model":"small-task","options":{"num_ctx":8192,"num_ctx":262144}}`, strings.Repeat("h", 32), 400}, {"/api/pull", `{"model":"small-task"}`, strings.Repeat("h", 32), 403}, {"/internal/config", `{}`, strings.Repeat("h", 32), 403}, {"/v1/chat/completions?model=agent-work", `{"model":"small-task"}`, strings.Repeat("h", 32), 403}, {"/v1/chat/completions", `{"model":"small-task:latest"}`, strings.Repeat("h", 32), 200}} {
		r := httptest.NewRequest("POST", tc.path, strings.NewReader(tc.body))
		r.Header.Set("Content-Type", "application/json")
		r.Header.Set("Authorization", "Bearer "+tc.key)
		r.Header.Set("Cookie", "private=value")
		w := httptest.NewRecorder()
		s.ServeHTTP(w, r)
		if w.Code != tc.status {
			t.Errorf("%s %s got %d want %d", tc.path, tc.body, w.Code, tc.status)
		}
	}
	if calls != 1 {
		t.Fatalf("unauthorized request reached backend: %d calls", calls)
	}
}
func TestJSONParser(t *testing.T) {
	for _, b := range []string{`{"x":1,"x":2}`, `{"x":{"a":1,"a":2}}`, `{} {}`, `{"a":`, strings.Repeat("[", 65) + "0" + strings.Repeat("]", 65)} {
		if _, err := decodeUniqueJSON([]byte(b)); err == nil {
			t.Fatal("accepted ambiguous/malformed JSON", b)
		}
	}
	if _, err := decodeUniqueJSON([]byte(`{"model":"small-task","messages":[{"role":"user","content":"hello"}]}`)); err != nil {
		t.Fatal(err)
	}
}
func TestDiscoveryIsScoped(t *testing.T) {
	s := testService(t, func(http.ResponseWriter, *http.Request) { t.Fatal("discovery forwarded") })
	for _, p := range []string{"/v1/models", "/api/tags"} {
		r := httptest.NewRequest("GET", p, nil)
		r.Header.Set("Authorization", "Bearer "+strings.Repeat("h", 32))
		w := httptest.NewRecorder()
		s.ServeHTTP(w, r)
		if w.Code != 200 || strings.Contains(w.Body.String(), "agent-work") || !strings.Contains(w.Body.String(), "small-task") {
			t.Fatal(w.Code, w.Body.String())
		}
	}
}
func TestInferenceQueueBound(t *testing.T) {
	s := testService(t, func(http.ResponseWriter, *http.Request) { t.Fatal("full queue forwarded") })
	for i := 0; i < 8; i++ {
		s.admitted <- struct{}{}
	}
	r := httptest.NewRequest("POST", "/api/chat", strings.NewReader(`{"model":"small-task"}`))
	r.Header.Set("Authorization", "Bearer "+strings.Repeat("h", 32))
	r.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	s.ServeHTTP(w, r)
	if w.Code != 429 {
		t.Fatal(w.Code)
	}
}
func TestWyomingFraming(t *testing.T) {
	data := `{"rate":16000,"width":2,"channels":1}`
	header, _ := json.Marshal(event{Type: "audio-chunk", DataLength: len(data), PayloadLength: 4})
	raw := append(header, '\n')
	raw = append(raw, []byte(data)...)
	raw = append(raw, 0, 1, 2, 3)
	e, err := readEvent(bufio.NewReader(bytes.NewReader(raw)))
	if err != nil || len(e.Payload) != 4 || parseFormat(e.Data) != (audioFormat{16000, 2, 1}) {
		t.Fatal(e, err)
	}
	var b bytes.Buffer
	if err = writeEvent(&b, e); err != nil {
		t.Fatal(err)
	}
	got, err := readEvent(bufio.NewReader(&b))
	if err != nil || !bytes.Equal(got.Payload, e.Payload) {
		t.Fatal(got, err)
	}
	for _, raw := range []string{`{"type":"audio-chunk","payload_length":-1}` + "\n", `{"type":"audio-chunk","payload_length":262145}` + "\n", strings.Repeat("x", 9000) + "\n"} {
		if _, err = readEvent(bufio.NewReaderSize(strings.NewReader(raw), 8192)); err == nil {
			t.Fatal("accepted invalid frame")
		}
	}
}
func TestWAVFormats(t *testing.T) {
	input := []byte{0, 1, 2, 3}
	b := wav(input, audioFormat{24000, 2, 1})
	pcm, f, err := unpackWAV(b)
	if err != nil || f.rate != 24000 || !bytes.Equal(pcm, input) {
		t.Fatal(f, err)
	}
	binary.LittleEndian.PutUint16(b[20:22], 3)
	binary.LittleEndian.PutUint16(b[34:36], 32)
	binary.LittleEndian.PutUint32(b[44:48], math.Float32bits(0.5))
	pcm, f, err = unpackWAV(b)
	if err != nil || f.width != 2 || len(pcm) != 2 {
		t.Fatal(f, err)
	}
	if _, _, err = unpackWAV(b[:46]); err == nil {
		t.Fatal("accepted truncated WAV")
	}
}
func TestVoiceDescribeSynthesisAndFreshConnection(t *testing.T) {
	s := testService(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/v1/audio/speech" {
			t.Error(r.URL.Path)
		}
		w.Header().Set("Content-Type", "audio/wav")
		w.Write(wav(make([]byte, 4800), audioFormat{24000, 2, 1}))
	})
	for i := 0; i < 2; i++ {
		server, client := net.Pipe()
		go s.voiceSession(context.Background(), server)
		client.SetDeadline(time.Now().Add(3 * time.Second))
		r := bufio.NewReader(client)
		if err := writeEvent(client, event{Type: "describe"}); err != nil {
			t.Fatal(err)
		}
		e, err := readEvent(r)
		if err != nil || e.Type != "info" {
			t.Fatal(e, err)
		}
		writeEvent(client, event{Type: "synthesize", Data: map[string]any{"text": "The kitchen temperature is twenty degrees."}})
		e, err = readEvent(r)
		if err != nil || e.Type != "audio-start" || parseFormat(e.Data).rate != 24000 {
			t.Fatal(e, err)
		}
		total := 0
		for {
			e, err = readEvent(r)
			if err != nil {
				t.Fatal(err)
			}
			if e.Type == "audio-stop" {
				break
			}
			total += len(e.Payload)
		}
		if total != 4800 {
			t.Fatal(total)
		}
		client.Close()
	}
}
func sendUtterance(t *testing.T, c net.Conn) {
	t.Helper()
	m := map[string]any{"rate": 16000, "width": 2, "channels": 1}
	for _, e := range []event{{Type: "transcribe"}, {Type: "audio-start", Data: m}, {Type: "audio-chunk", Data: m, Payload: make([]byte, 3200)}, {Type: "audio-stop"}} {
		if err := writeEvent(c, e); err != nil {
			t.Fatal(err)
		}
	}
}
func TestTranscriptionAndBackendFailure(t *testing.T) {
	fail := false
	s := testService(t, func(w http.ResponseWriter, r *http.Request) {
		if fail {
			http.Error(w, "backend down", 503)
			return
		}
		if err := r.ParseMultipartForm(1 << 20); err != nil {
			t.Error(err)
		}
		if r.FormValue("model") != "speech-stt" {
			t.Error("wrong model")
		}
		writeJSON(w, 200, map[string]string{"text": "Turn on the kitchen light."})
	})
	server, client := net.Pipe()
	go s.voiceSession(context.Background(), server)
	defer client.Close()
	client.SetDeadline(time.Now().Add(4 * time.Second))
	r := bufio.NewReader(client)
	sendUtterance(t, client)
	e, err := readEvent(r)
	if err != nil || e.Type != "transcript" {
		t.Fatal(e, err)
	}
	fail = true
	sendUtterance(t, client)
	e, err = readEvent(r)
	if err != nil || e.Type != "error" {
		t.Fatal(e, err)
	}
	fail = false
	sendUtterance(t, client)
	e, err = readEvent(r)
	if err != nil || e.Type != "transcript" {
		t.Fatal(e, err)
	}
}
func TestDisconnectCancelsBackend(t *testing.T) {
	started := make(chan struct{})
	canceled := make(chan struct{})
	s := testService(t, func(w http.ResponseWriter, r *http.Request) {
		_, _ = io.ReadAll(r.Body)
		close(started)
		<-r.Context().Done()
		close(canceled)
	})
	server, client := net.Pipe()
	go s.voiceSession(context.Background(), server)
	sendUtterance(t, client)
	select {
	case <-started:
	case <-time.After(2 * time.Second):
		t.Fatal("backend not started")
	}
	client.Close()
	select {
	case <-canceled:
	case <-time.After(2 * time.Second):
		t.Fatal("disconnect did not cancel upstream")
	}
}
func TestSpeechAdmission(t *testing.T) {
	s := testService(t, func(http.ResponseWriter, *http.Request) { t.Fatal("busy STT forwarded") })
	s.stt <- struct{}{}
	if _, err := s.transcribe(context.Background(), wav(make([]byte, 3200), audioFormat{16000, 2, 1})); err == nil {
		t.Fatal("unbounded speech admission")
	}
}
