# Relay

The relay starts an HTTPS API server on port 8080 by default. It creates
`key.pem` and `cert.pem` in `RELAY_CONFIG_DIR`, or beside the executable if
that variable is unset.

The relay stores its API key in `api-key` in that same directory. It generates
and prints a key on first startup. Run `obs-relay --generate-api-key` with the
same `RELAY_CONFIG_DIR` to replace the key and print the new one; running relay
servers use the replacement for subsequent requests. Send the key in the
`Authorization` header for each request.

The API maintains an in-memory feed list and starts backends asynchronously.
The list is lost when the relay exits.
Protocols are case-insensitive: `MoQ`, `SRT`, `WebRTC`, `RTSP`, `RTMP`,
and `SRTLA`.

- `POST /feeds` with JSON `{"protocol":"SRT","streamKey":"example"}` adds a
  feed and returns `202` with a `link` to the requested feed. Duplicate stream
  keys return `409`. The link is returned immediately; check the feed status to
  see when startup completes.
- `DELETE /feeds` with the same JSON starts removal and returns `202`, or `404`
  if it does not exist.
- `GET /feeds` returns a JSON array of objects containing `protocol`,
  `streamKey`, `link`, and `status` (`starting`, `running`, `stopping`, or
  `failed`). Failed entries include an `error` field.
- `GET /status` returns `{"status":"ok"}` while the authenticated API is
  responding; it does not check the health of each feed.

The relay discovers its public IPv4 address using `curl` and
`https://api.ipify.org` at startup and refreshes it every five minutes. The
Docker image includes curl; other installations must provide it on `PATH`.
Discovery runs asynchronously. Until
it succeeds, links use the request's `Host` header; existing feed links update
when an address is discovered. If a later lookup fails, the last discovered
address is retained.

Optionally set `RELAY_PUBLIC_HOST` to an IP address or domain (without a scheme
or port) to override discovery. Links use the configured protocol ports, so Docker
mappings and any external firewall must allow those ports. If Docker maps a
different external port, clients must adjust the link. SRTLA uses the returned
server URL and the stream key separately. The detected outbound address may not
be reachable from the internet on every network, especially behind NAT or a VPN.

Invalid request bodies return `400`, and an incorrect API key returns `401`.

To try the API interactively, run `python tools/relay_api_tester.py` from the
repository root. Enter the relay host, port, and API key, then use the feed
buttons to send requests. Like the plugin API tester, this tool accepts the
relay's self-signed TLS certificate without verifying its identity.

Build the Docker image from the repository root so the shared certificate code
is included:

```sh
docker build -f relay/Dockerfile -t obs-relay .
```

On first startup, the relay creates `relay.yml` in `RELAY_CONFIG_DIR`, or
beside the executable when that variable is unset. Edit this file and restart
the relay to change its listening ports. Omitted ports use the defaults below;
invalid or unknown settings prevent startup. The MediaMTX API listens only on
localhost. The relay manages `mediamtx.yml` separately; do not edit that file.

```yaml
ports:
  api: 8080
  srtla: 5000
  srt: 8890
  rtsp: 8554
  rtp: 8000
  rtcp: 8001
  rtmp: 1935
  webrtc: 8889
  moq: 8892
  moq_quic: 8893
  media_api: 9997
```

All ports must be integers between 1 and 65535. The relay uses the configured
ports in publish links. Make sure the chosen ports are available and that
Docker port mappings and firewall rules match. In Docker, mount a persistent
directory at `/var/lib/relay` to edit and retain `relay.yml`, certificates,
and the API key. For example, after creating that directory and its config:

```sh
docker run -v /path/to/relay-data:/var/lib/relay -p 8081:8081 obs-relay
```

Set `ports.api: 8081` in the mounted file for this example. The image's
`EXPOSE` metadata lists the default ports and does not publish them.

`FeedContext` wraps one feed's protocol, stream key, address, and asynchronous
start/stop lifecycle. It uses a shared MediaMTX manager for supported media
protocols, or a go-irl process for SRTLA, and reports state through Qt signals.
Create contexts and their managers on the same Qt thread, keep a context alive
until it has stopped, and use unique stream keys per MediaMTX manager. Each
SRTLA context owns a go-irl process using the configured shared SRTLA port.
The API owns and manages its contexts.

Request handlers run on HTTP worker threads. For handlers that start or
stop Qt-owned process managers, `API_Endpoint::postToEventLoop` queues a
callback on the application thread and returns immediately. Capture request
values by value, not references to the HTTP request or response: the callback
may run after the handler returns. Start processes asynchronously there and use
their signals to record completion or failure; do not block the Qt event loop.
A successful queue operation only means the callback was accepted, not that
the feed is ready. A handler that returns before completion should use an
appropriate response such as `202 Accepted` and expose the eventual status.
