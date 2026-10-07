package main

import "testing"

func TestRolePresetsAndClientOverrides(t *testing.T) {
	for _, path := range []string{"/api/chat", "/v1/chat/completions"} {
		obj := map[string]any{"model": "small-task"}
		target := obj
		if path == "/api/chat" {
			target = map[string]any{"temperature": 0.25, "min_p": 0.1}
			obj["options"] = target
		} else {
			obj["temperature"] = 0.25
			obj["min_p"] = 0.1
		}
		if err := applyPreset(obj, "small-task", path); err != nil {
			t.Fatal(err)
		}
		if target["temperature"] != 0.25 || target["min_p"] != 0.1 || target["top_k"] != float64(20) || target["repeat_penalty"] != float64(1) {
			t.Fatalf("incorrect settings: %v", target)
		}
	}
	if rolePresets["chat-roleplay"]["temperature"] == rolePresets["small-task"]["temperature"] {
		t.Fatal("roles lost separate sampling defaults")
	}
	if err := applyPreset(map[string]any{"options": "invalid"}, "small-task", "/api/chat"); err == nil {
		t.Fatal("invalid options accepted")
	}
}
