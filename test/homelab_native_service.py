"""Exercise a real lemond without models, accelerator access or network downloads."""

import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="native-consumer-") as directory:
        root = Path(directory)
        ports = []
        for _ in range(3):
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                ports.append(listener.getsockname()[1])
        manager_port, consumer_port, voice_port = ports
        config = {
            "host": "127.0.0.1",
            "port": manager_port,
            "offline": True,
            "broadcast": False,
            "auto_check_model_updates": False,
            "telemetry": {"enabled": False},
            "consumer": {
                "enabled": True,
                "host": "127.0.0.1",
                "port": consumer_port,
                "wyoming_host": "127.0.0.1",
                "wyoming_port": voice_port,
                "critical_models": [],
            },
        }
        (root / "config.json").write_text(json.dumps(config))
        env = os.environ.copy()
        for key in (
            "LEMONADE_API_KEY",
            "LEMONADE_ADMIN_API_KEY",
            "LEMONADE_DEFAULTS_PATH",
        ):
            env.pop(key, None)
        env.update(HOME=str(root), HF_HOME=str(root / "hf"))
        with (root / "server.log").open("w") as log:
            process = subprocess.Popen(
                [str(binary), str(root), str(root)], stdout=log, stderr=log, env=env
            )

            def request(port, path, body=None):
                data = json.dumps(body).encode() if body is not None else None
                req = urllib.request.Request(
                    f"http://127.0.0.1:{port}{path}",
                    data=data,
                    headers={"Content-Type": "application/json"},
                )
                with urllib.request.urlopen(req, timeout=5) as response:
                    return json.load(response)

            try:
                for _ in range(100):
                    if process.poll() is not None:
                        raise RuntimeError("Native server exited during startup")
                    try:
                        request(consumer_port, "/ready")
                        break
                    except OSError:
                        time.sleep(0.1)
                else:
                    raise RuntimeError("Native consumer never became ready")
                for prefix in ("/api/v0/", "/api/v1/", "/v0/", "/v1/"):
                    assert request(manager_port, prefix + "ready")["ready"]
                    assert request(manager_port, prefix + "service")["configuration"][
                        "enabled"
                    ]
                    assert len(request(consumer_port, prefix + "models")["data"]) == 5
                assert len(request(consumer_port, "/openapi.json")["paths"]) > 10
                assert len(request(consumer_port, "/api/tags")["models"]) == 5
                # Saved configuration changes do not silently alter bound listeners.
                request(
                    manager_port,
                    "/internal/set",
                    {"consumer": {"tts_voice": "af_bella"}},
                )
                assert (
                    request(manager_port, "/internal/config")["consumer"]["tts_voice"]
                    == "af_bella"
                )
                assert (
                    request(consumer_port, "/v1/service")["configuration"]["tts_voice"]
                    == "af_heart"
                )
                try:
                    request(consumer_port, "/v1/load", {"model": "small-task"})
                    raise AssertionError("Consumer administration exposed")
                except urllib.error.HTTPError as error:
                    assert error.code == 403
                with socket.create_connection(
                    ("127.0.0.1", voice_port), timeout=5
                ) as voice:
                    voice.sendall(b'{"type":"describe"}\n')
                    info = json.loads(voice.makefile("rb").readline())
                    assert (
                        info["data"]["tts"][0]["voices"] == []
                    )  # No model fixture: never invent installed voices.
            finally:
                process.terminate()
                try:
                    process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    raise AssertionError("Native shutdown hung")
            assert process.returncode == 0, "Native shutdown failed"
        print(
            "Real lemond consumer/manager discovery, shared config, voice and SIGTERM passed"
        )


if __name__ == "__main__":
    main()
