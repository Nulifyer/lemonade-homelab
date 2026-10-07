package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"mime/multipart"
	"net"
	"net/http"
	"strings"
	"time"
)

type event struct {
	Type          string         `json:"type"`
	Data          map[string]any `json:"data,omitempty"`
	DataLength    int            `json:"data_length,omitempty"`
	PayloadLength int            `json:"payload_length,omitempty"`
	Payload       []byte         `json:"-"`
}

func readEvent(r *bufio.Reader) (event, error) {
	line, err := r.ReadSlice('\n')
	if err != nil {
		return event{}, err
	}
	var e event
	if err = json.Unmarshal(line, &e); err != nil {
		return e, err
	}
	if e.DataLength < 0 || e.DataLength > 65536 || e.PayloadLength < 0 || e.PayloadLength > 262144 {
		return e, errors.New("Wyoming frame too large")
	}
	if e.Data == nil {
		e.Data = map[string]any{}
	}
	if e.DataLength > 0 {
		b := make([]byte, e.DataLength)
		if _, err = io.ReadFull(r, b); err != nil {
			return e, err
		}
		var extra map[string]any
		if err = json.Unmarshal(b, &extra); err != nil {
			return e, err
		}
		for k, v := range extra {
			e.Data[k] = v
		}
	}
	if e.PayloadLength > 0 {
		e.Payload = make([]byte, e.PayloadLength)
		_, err = io.ReadFull(r, e.Payload)
	}
	return e, err
}
func writeEvent(w io.Writer, e event) error {
	e.DataLength = 0
	e.PayloadLength = len(e.Payload)
	header, err := json.Marshal(e)
	if err != nil {
		return err
	}
	header = append(header, '\n')
	if _, err = w.Write(header); err != nil {
		return err
	}
	if len(e.Payload) > 0 {
		_, err = w.Write(e.Payload)
	}
	return err
}
func voiceInfo(voice string) map[string]any {
	artifact := func(name, url string) map[string]any {
		return map[string]any{"name": name, "description": "Local compiled speech service", "attribution": map[string]string{"name": "Homelab", "url": url}, "installed": true, "version": "0.1.0"}
	}
	asr := artifact("redux", "https://github.com/mudler/parakeet.cpp")
	model := artifact("speech-stt", "https://huggingface.co/moondream/parakeet-redux")
	model["languages"] = []string{"en"}
	asr["models"] = []any{model}
	asr["supports_transcript_streaming"] = false
	asr["requires_external_vad"] = true
	tts := artifact("kokoro", "https://github.com/lucasjinreal/Kokoros")
	v := artifact(voice, "https://huggingface.co/hexgrad/Kokoro-82M")
	v["languages"] = []string{"en"}
	tts["voices"] = []any{v}
	tts["supports_synthesize_streaming"] = false
	return map[string]any{"asr": []any{asr}, "tts": []any{tts}, "wake": []any{}, "handle": []any{}}
}
func (s *service) serveVoice(ctx context.Context, l net.Listener) {
	for {
		c, err := l.Accept()
		if err != nil {
			return
		}
		select {
		case s.voiceClients <- struct{}{}:
			go func() { defer func() { <-s.voiceClients }(); s.voiceSession(ctx, c) }()
		default:
			c.Close()
		}
	}
}
func (s *service) voiceSession(parent context.Context, c net.Conn) {
	defer c.Close()
	ctx, cancel := context.WithCancel(parent)
	defer cancel()
	incoming := make(chan event, 16)
	go func() {
		defer cancel()
		r := bufio.NewReaderSize(c, 8192)
		for {
			_ = c.SetReadDeadline(time.Now().Add(120 * time.Second))
			e, err := readEvent(r)
			if err != nil {
				return
			}
			select {
			case incoming <- e:
			case <-ctx.Done():
				return
			}
		}
	}()
	go func() { <-ctx.Done(); c.Close() }()
	var audio []byte
	var f audioFormat
	transcribing := false
	started := false
	send := func(e event) error { _ = c.SetWriteDeadline(time.Now().Add(10 * time.Second)); return writeEvent(c, e) }
	fail := func(code string) {
		s.failures.Add(1)
		s.logEvent(slog.LevelWarn, "wyoming_failure", "reason", code)
		_ = send(event{Type: "error", Data: map[string]any{"code": code, "text": "Speech request failed. Start a fresh request."}})
	}
	for {
		select {
		case <-ctx.Done():
			return
		case e := <-incoming:
			switch e.Type {
			case "describe":
				if send(event{Type: "info", Data: voiceInfo(s.cfg.voice)}) != nil {
					return
				}
			case "select-program":
			case "transcribe":
				audio = nil
				transcribing = true
				started = false
			case "audio-start":
				if !transcribing {
					fail("invalid-state")
					return
				}
				f = parseFormat(e.Data)
				if !f.valid() {
					fail("unsupported-audio")
					return
				}
				started = true
			case "audio-chunk":
				if !started || !transcribing || parseFormat(e.Data) != f || len(audio)+len(e.Payload) > 2*1024*1024 {
					fail("invalid-audio")
					return
				}
				audio = append(audio, e.Payload...)
			case "audio-stop":
				if !transcribing || !started || len(audio) == 0 || len(audio)%(f.width*f.channels) != 0 {
					fail("invalid-audio")
					return
				}
				text, err := s.transcribe(ctx, wav(audio, f))
				audio = nil
				transcribing = false
				started = false
				if err != nil {
					fail("transcription-failed")
					continue
				}
				if send(event{Type: "transcript", Data: map[string]any{"text": text}}) != nil {
					return
				}
			case "synthesize":
				text, ok := e.Data["text"].(string)
				if !ok || len(text) == 0 || len(text) > 8192 {
					fail("invalid-text")
					continue
				}
				if format, _ := e.Data["text_format"].(string); format != "" && format != "text" {
					fail("unsupported-text-format")
					continue
				}
				voice := s.cfg.voice
				if v, ok := e.Data["voice"].(map[string]any); ok {
					if name, _ := v["name"].(string); name != "" && name != voice {
						fail("unsupported-voice")
						continue
					}
				}
				b, err := s.synthesize(ctx, text, voice)
				if err != nil {
					fail("synthesis-failed")
					continue
				}
				pcm, f, err := unpackWAV(b)
				if err != nil {
					fail("invalid-backend-audio")
					continue
				}
				metadata := map[string]any{"rate": f.rate, "width": f.width, "channels": f.channels}
				if send(event{Type: "audio-start", Data: metadata}) != nil {
					return
				}
				for len(pcm) > 0 {
					n := 4096
					if n > len(pcm) {
						n = len(pcm)
					}
					if send(event{Type: "audio-chunk", Data: metadata, Payload: pcm[:n]}) != nil {
						return
					}
					pcm = pcm[n:]
				}
				if send(event{Type: "audio-stop"}) != nil {
					return
				}
			}
		}
	}
}
func parseFormat(m map[string]any) audioFormat {
	n := func(k string) int { v, _ := m[k].(float64); return int(v) }
	return audioFormat{n("rate"), n("width"), n("channels")}
}
func (s *service) speechRequest(ctx context.Context, path, contentType string, b []byte, slot chan struct{}) (data []byte, requestErr error) {
	started := time.Now()
	state := s.newAudit()
	model := "speech-tts"
	if path == "/api/v1/audio/transcriptions" {
		model = "speech-stt"
	}
	status := 0
	s.logEvent(slog.LevelInfo, "speech_started", "request_id", state.id, "model", model)
	defer func() {
		level := slog.LevelInfo
		reason := ""
		if requestErr != nil {
			level = slog.LevelWarn
			reason = errorKind(requestErr)
		}
		s.logEvent(level, "speech_request", "request_id", state.id, "model", model, "status", status, "duration_ms", time.Since(started).Milliseconds(), "response_bytes", len(data), "reason", reason)
	}()
	select {
	case slot <- struct{}{}:
		defer func() { <-slot }()
	default:
		return nil, errors.New("speech queue full")
	}
	ctx, cancel := context.WithTimeout(ctx, 60*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, "POST", strings.TrimSuffix(s.cfg.upstream.String(), "/")+path, bytes.NewReader(b))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", contentType)
	if s.cfg.backendKey != "" {
		req.Header.Set("Authorization", "Bearer "+s.cfg.backendKey)
	}
	resp, err := s.client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	status = resp.StatusCode
	if resp.StatusCode != 200 {
		return nil, errors.New("speech backend failed")
	}
	data, err = io.ReadAll(io.LimitReader(resp.Body, 16*1024*1024+1))
	if len(data) > 16*1024*1024 {
		return nil, errors.New("speech response too large")
	}
	return data, err
}
func (s *service) transcribe(ctx context.Context, b []byte) (string, error) {
	var body bytes.Buffer
	w := multipart.NewWriter(&body)
	_ = w.WriteField("model", "speech-stt")
	_ = w.WriteField("language", "en")
	_ = w.WriteField("response_format", "json")
	f, err := w.CreateFormFile("file", "audio.wav")
	if err != nil {
		return "", err
	}
	_, _ = f.Write(b)
	_ = w.Close()
	data, err := s.speechRequest(ctx, "/api/v1/audio/transcriptions", w.FormDataContentType(), body.Bytes(), s.stt)
	if err != nil {
		return "", err
	}
	var reply struct {
		Text string `json:"text"`
	}
	err = json.Unmarshal(data, &reply)
	return reply.Text, err
}
func (s *service) synthesize(ctx context.Context, text, voice string) ([]byte, error) {
	body, _ := json.Marshal(map[string]any{"model": "speech-tts", "input": text, "voice": voice, "response_format": "wav", "speed": 1.0})
	return s.speechRequest(ctx, "/api/v1/audio/speech", "application/json", body, s.tts)
}
