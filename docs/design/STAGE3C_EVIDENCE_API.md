# Stage 3C evidence API

`tools/idw-evidence-api.py` is a read-only HTTP wrapper around
`tests/physical/haswell-evidence.sh`.

It does not expose a shell, accept command arguments, read arbitrary files, or
change the GPU driver. It only runs the fixed collector and returns its logs.

## Run on the Haswell host

```sh
cd /path/to/intel_win_onlinux
read -rsp 'Evidence API token: ' IDW_EVIDENCE_TOKEN; echo
export IDW_EVIDENCE_TOKEN
python3 tools/idw-evidence-api.py --host 127.0.0.1 --port 8000
```

The server prints the token if `IDW_EVIDENCE_TOKEN` is not set. Keep the
server bound to `127.0.0.1` and expose it through a private tunnel.

## Private access options

### Tailscale

Use Tailscale on both the host and the VM. Run the API on the host's tailnet
address or bind locally and use `tailscale serve`:

```sh
python3 tools/idw-evidence-api.py --host 127.0.0.1 --port 8000
tailscale serve --bg 8000
```

The resulting `https://<host>.<tailnet>.ts.net` URL is reachable only inside
the tailnet. Restrict the tailnet ACL to the VM and the host.

### Cloudflare Quick Tunnel

If Tailscale is not available:

```sh
cloudflared tunnel --url http://127.0.0.1:8000
```

Cloudflare prints a temporary `https://...trycloudflare.com` URL. The API
token still protects `/collect`. Stop `cloudflared` and the API after the run.

Do not forward port 8000 on the router and do not expose it directly to the
public internet.

## Call it

```sh
curl -sS \
  -H "Authorization: Bearer $IDW_EVIDENCE_TOKEN" \
  -X POST "$EVIDENCE_API_URL/collect" \
  -o evidence.json
```

`GET /health` is unauthenticated. `POST /collect` requires the bearer token.

The JSON response contains:

- `result`: `PASS`, `TARGET_GPU_NOT_PRESENT`, `TARGET_GPU_MISMATCH`,
  `TARGET_GPU_NOT_BOUND_TO_I915`, or `RENDER_NODE_ABSENT`
- `exit_code`: collector exit status
- `run_id`
- `stdout` and `stderr`
- `files`: the evidence logs

The server serializes collection requests; a second request while one is
running receives `409`.

## Security boundary

- The API only runs `haswell-evidence.sh`.
- The collector reads `lspci`, sysfs, `/proc/modules`, and `/dev/dri`.
- The API does not run `modprobe`, unbind, `setpci`, MMIO, BAR, reset, or
  VFIO operations.
- Use a short-lived token and stop the server and tunnel after the run.
- Keep the host on an isolated test network.
