package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"math"
)

type audioFormat struct{ rate, width, channels int }

func (f audioFormat) valid() bool {
	return f.rate >= 8000 && f.rate <= 48000 && f.width == 2 && (f.channels == 1 || f.channels == 2)
}
func wav(pcm []byte, f audioFormat) []byte {
	b := new(bytes.Buffer)
	b.WriteString("RIFF")
	_ = binary.Write(b, binary.LittleEndian, uint32(36+len(pcm)))
	b.WriteString("WAVEfmt ")
	for _, n := range []any{uint32(16), uint16(1), uint16(f.channels), uint32(f.rate), uint32(f.rate * f.channels * f.width), uint16(f.channels * f.width), uint16(f.width * 8)} {
		_ = binary.Write(b, binary.LittleEndian, n)
	}
	b.WriteString("data")
	_ = binary.Write(b, binary.LittleEndian, uint32(len(pcm)))
	b.Write(pcm)
	return b.Bytes()
}
func unpackWAV(b []byte) ([]byte, audioFormat, error) {
	bad := errors.New("unsupported or malformed speech WAV")
	if len(b) < 12 || string(b[:4]) != "RIFF" || string(b[8:12]) != "WAVE" {
		return nil, audioFormat{}, bad
	}
	f := audioFormat{}
	format := uint16(0)
	var pcm []byte
	for offset := 12; offset+8 <= len(b); {
		size := int(binary.LittleEndian.Uint32(b[offset+4 : offset+8]))
		start := offset + 8
		// Kokoro emits a streaming WAV: its final data size is unknown until EOF.
		// The HTTP reader has already bounded the complete response. Other chunks
		// must still have their declared length, so truncated WAVs remain errors.
		if size == math.MaxUint32 && string(b[offset:offset+4]) == "data" {
			size = len(b) - start
		}
		if size > len(b)-start {
			return nil, f, bad
		}
		chunk := b[start : start+size]
		switch string(b[offset : offset+4]) {
		case "fmt ":
			if size < 16 {
				return nil, f, bad
			}
			format = binary.LittleEndian.Uint16(chunk[:2])
			f = audioFormat{int(binary.LittleEndian.Uint32(chunk[4:8])), int(binary.LittleEndian.Uint16(chunk[14:16])) / 8, int(binary.LittleEndian.Uint16(chunk[2:4]))}
		case "data":
			pcm = chunk
		}
		offset = start + size + (size % 2)
	}
	if f.rate < 8000 || f.rate > 48000 || f.channels < 1 || f.channels > 2 || len(pcm) == 0 {
		return nil, f, bad
	}
	if format == 3 && f.width == 4 {
		out := make([]byte, len(pcm)/2)
		if len(pcm)%4 != 0 {
			return nil, f, bad
		}
		for i := 0; i < len(pcm)/4; i++ {
			x := float64(math.Float32frombits(binary.LittleEndian.Uint32(pcm[i*4 : i*4+4])))
			if math.IsNaN(x) || math.IsInf(x, 0) {
				return nil, f, bad
			}
			x = math.Max(-1, math.Min(1, x))
			binary.LittleEndian.PutUint16(out[i*2:], uint16(int16(x*32767)))
		}
		pcm = out
		f.width = 2
	} else if format != 1 || f.width != 2 {
		return nil, f, bad
	}
	if len(pcm)%(f.width*f.channels) != 0 {
		return nil, f, bad
	}
	return pcm, f, nil
}
