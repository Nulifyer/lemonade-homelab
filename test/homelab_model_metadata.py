"""Exercise model catalog metadata without downloading or loading any model."""

import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="homelab-metadata-") as directory:
        root = Path(directory)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        (root / "config.json").write_text(
            json.dumps(
                {
                    "config_version": 2,
                    "port": port,
                    "host": "127.0.0.1",
                    "ctx_size": 4096,
                    "offline": True,
                    "broadcast": False,
                    "disable_model_filtering": True,
                    "auto_check_model_updates": False,
                }
            )
        )
        env = os.environ.copy()
        for key in ("LEMONADE_API_KEY", "LEMONADE_ADMIN_API_KEY"):
            env.pop(key, None)
        env["HF_HOME"] = str(root / "hf")
        env["HOME"] = str(root)
        with (root / "server.log").open("w") as log:
            process = subprocess.Popen(
                [str(binary), str(root), str(root)],
                stdout=log,
                stderr=subprocess.STDOUT,
                env=env,
            )

            def request(path, body=None):
                req = urllib.request.Request(
                    f"http://127.0.0.1:{port}{path}",
                    data=json.dumps(body).encode() if body is not None else None,
                    headers={"Content-Type": "application/json"},
                )
                with urllib.request.urlopen(req, timeout=10) as response:
                    return json.load(response)

            try:
                for _ in range(100):
                    if process.poll() is not None:
                        raise RuntimeError("Metadata test server exited during startup")
                    try:
                        request("/api/v1/health")
                        break
                    except OSError:
                        time.sleep(0.1)
                else:
                    raise RuntimeError("Metadata test server did not become ready")

                for name, recipe in (
                    ("speech", "kokoro"),
                    ("transcription", "parakeet"),
                    ("image", "sd-cpp"),
                    ("chat", "llamacpp"),
                ):
                    request(
                        "/api/v1/models/register",
                        {
                            "model_name": "user.metadata-" + name,
                            "checkpoint": "example/metadata-fixture:model.gguf",
                            "recipe": recipe,
                        },
                    )
                    model = request("/api/v1/models/metadata-" + name)
                    if name == "chat":
                        assert model["context_length"] == 4096, model
                    else:
                        assert "context_length" not in model, model
                request(
                    "/internal/aliases",
                    {"alias": "speech-fixture", "target": "user.metadata-speech"},
                )
                models = request("/api/v1/models?show_all=true")["data"]
                aliases = [m for m in models if m["id"] == "speech-fixture"]
                assert len(aliases) == 1, aliases
                assert aliases[0]["alias_of"] == "metadata-speech", aliases
                assert (
                    request("/api/v1/models/speech-fixture")["alias_of"]
                    == "metadata-speech"
                )
                assert not request("/api/v1/health")["all_models_loaded"]
                print("Metadata acceptance passed: modality contexts and alias targets")
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)


if __name__ == "__main__":
    main()
