package main

import (
	_ "embed"
	"encoding/json"
	"errors"
)

//go:embed presets.json
var presetJSON []byte

var rolePresets = func() map[string]map[string]any {
	var p map[string]map[string]any
	if err := json.Unmarshal(presetJSON, &p); err != nil {
		panic(err)
	}
	return p
}()

func applyPreset(obj map[string]any, model, path string) error {
	defaults := rolePresets[model]
	if defaults == nil || path == "/api/show" {
		return nil
	}
	target := obj
	if path == "/api/chat" {
		if existing, present := obj["options"]; present {
			var ok bool
			target, ok = existing.(map[string]any)
			if !ok {
				return errors.New("options must be an object")
			}
		} else {
			target = map[string]any{}
			obj["options"] = target
		}
	}
	for key, value := range defaults {
		if path == "/api/chat" && key == "max_tokens" {
			key = "num_predict"
		}
		if _, supplied := target[key]; !supplied {
			target[key] = value
		}
	}
	return nil
}
