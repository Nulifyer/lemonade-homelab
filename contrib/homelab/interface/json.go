package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"strconv"
)

func uintString(v uint64) string { return strconv.FormatUint(v, 10) }

func decodeUniqueJSON(data []byte) (any, error) {
	d := json.NewDecoder(bytes.NewReader(data))
	d.UseNumber()
	var read func(int) (any, error)
	read = func(depth int) (any, error) {
		if depth > 64 {
			return nil, errors.New("JSON too deep")
		}
		t, err := d.Token()
		if err != nil {
			return nil, err
		}
		delim, ok := t.(json.Delim)
		if !ok {
			return t, nil
		}
		if delim == '{' {
			m := map[string]any{}
			for d.More() {
				k, err := d.Token()
				if err != nil {
					return nil, err
				}
				name, ok := k.(string)
				if !ok {
					return nil, errors.New("invalid object key")
				}
				if _, exists := m[name]; exists {
					return nil, errors.New("duplicate object key")
				}
				v, err := read(depth + 1)
				if err != nil {
					return nil, err
				}
				m[name] = v
			}
			_, err = d.Token()
			return m, err
		}
		if delim == '[' {
			a := []any{}
			for d.More() {
				v, err := read(depth + 1)
				if err != nil {
					return nil, err
				}
				a = append(a, v)
			}
			_, err = d.Token()
			return a, err
		}
		return nil, errors.New("invalid JSON delimiter")
	}
	v, err := read(0)
	if err != nil {
		return nil, err
	}
	if _, err = d.Token(); err != io.EOF {
		return nil, errors.New("trailing JSON")
	}
	return v, nil
}
