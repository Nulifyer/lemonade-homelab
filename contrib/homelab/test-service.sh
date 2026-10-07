#!/usr/bin/env bash
set -euo pipefail
image=${1:?provide the built image}
container=lemonade-native-smoke
fixture=$(mktemp)
cleanup() {
    docker rm -f "$container" >/dev/null 2>&1 || true
    rm -f "$fixture"
}
trap cleanup EXIT
printf '%s\n' '{"consumer":{"enabled":true,"critical_models":[]},"offline":true,"auto_check_model_updates":false,"telemetry":{"enabled":false}}' > "$fixture"
chmod 644 "$fixture"
docker run --rm --network none "$image" --help >/dev/null
docker run --detach --name "$container" --network none \
    --mount "type=bind,source=$fixture,target=/etc/lemonade-defaults.json,readonly" \
    --env LEMONADE_DEFAULTS_PATH=/etc/lemonade-defaults.json "$image" >/dev/null
ready=false
for _ in $(seq 1 30); do
    if docker exec "$container" curl --fail --silent --max-time 2 http://127.0.0.1:8080/ready >/dev/null; then
        ready=true
        break
    fi
    sleep 2
done
if [[ "$ready" != true ]]; then
    docker logs "$container"
    exit 1
fi
docker exec "$container" curl --fail --silent http://127.0.0.1:8080/v1/models | python3 -c 'import json,sys; assert len(json.load(sys.stdin)["data"])==5'
docker exec "$container" curl --fail --silent http://127.0.0.1:13305/v1/service | python3 -c 'import json,sys; assert json.load(sys.stdin)["configuration"]["critical_models"]==[]'
[[ $(docker exec "$container" cat /proc/1/comm) == lemond ]]
docker stop --time 110 "$container" >/dev/null
[[ $(docker inspect --format '{{.State.ExitCode}}' "$container") == 0 ]]
printf '%s\n' 'Native lemond startup, discovery, readiness and SIGTERM passed without models or hardware.'
