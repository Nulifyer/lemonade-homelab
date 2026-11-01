#!/usr/bin/env bash
set -euo pipefail
image=${1:?image required}
trial_id=
cleanup() {
  if [[ -n "$trial_id" ]]; then docker rm -vf "$trial_id" >/dev/null 2>&1 || true; fi
}
trap cleanup EXIT

docker run --rm "$image" --version | grep -F '0.1.0'
docker run --rm --entrypoint node "$image" --version | grep -F 'v22.22.1'
docker run --rm --entrypoint pi "$image" --version | grep -F '0.80.5'
docker run --rm --entrypoint pi "$image" --list-models homelab | grep -F 'agent-work'
trial_id=$(docker run -d --init --memory 4g --cpus 2 --cap-drop ALL \
  --security-opt no-new-privileges "$image")
ready=false
for ((attempt=0; attempt<60; attempt++)); do
  if docker exec "$trial_id" node -e \
    "fetch('http://127.0.0.1:3773/',{signal:AbortSignal.timeout(3000)}).then(r=>process.exit(r.ok?0:1)).catch(()=>process.exit(1))" \
    >/dev/null 2>&1; then ready=true; break; fi
  sleep 2
done
if [[ "$ready" != true ]]; then
  docker inspect --format '{{.State.Status}} {{.State.ExitCode}} {{.State.Error}}' "$trial_id"
  exit 1
fi
# Headless startup prints one-time pairing credentials. Keep those logs private.
docker exec "$trial_id" node -e \
  "const fs=require('fs');fs.writeFileSync('/data/write-check','ok');fs.writeFileSync('/workspace/write-check','ok');if(process.getuid()===0)process.exit(1)"
docker stop --time 15 "$trial_id" >/dev/null
code=$(docker inspect --format '{{.State.ExitCode}}' "$trial_id")
oom=$(docker inspect --format '{{.State.OOMKilled}}' "$trial_id")
echo "Explicit stop: exit=$code oom=$oom"
# The official 0.1.0 CLI maps an intentional SIGTERM interruption to 130.
[[ "$oom" == false ]]
[[ "$code" == 0 || "$code" == 130 || "$code" == 143 ]]
echo 'T3/Pi versions, model discovery, non-root HTTP startup, persistent writes and shutdown passed'
