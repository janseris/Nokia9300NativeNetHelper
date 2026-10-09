# Net Helper 9300

A native (Symbian C++) helper for the Java apps on the **Nokia 9300** (Series 80 2.0, Symbian 7.0s EKA1).

Java MIDlets on the 9300 can't keep connections open: every `HttpConnection` is a new TCP connection
and, for HTTPS, a new TLS handshake (~0.5–1 s extra per request, measured), and unsigned MIDlets may
not open `socket://` to ports 80/443. They may, however, talk to `http://127.0.0.1`. The helper runs
on the phone, listens there and will do the network work for them over kept-open connections.

## Status

- **Step 1 (0.1):** the app listens on `127.0.0.1:8123` and answers with a short text. Probe 3.1 →
  *Test Net Helper*: ~80 ms per request from a MIDlet to the helper.
- **Step 2 (0.2):** `GET /fetch?u=<percent-encoded URL>` fetches the URL over kept-open HTTP/1.1
  connections (up to 4, one per server; TLS through `CSecureSocket`, i.e. the phone's patched
  `SSLADAPTOR.dll`, with SNI) and answers with the server's status code, `Content-Type` and body.
  - optional request header `X-Ua`: the User-Agent to send (otherwise the helper's own);
  - `X-Helper` response header: `conn=reused|new dns= connect= tls= first-byte= total=` (ms);
  - on failure: HTTP 502 with `X-Helper-Error: <Symbian error> <step>`.
  - The network work runs in its own thread (blocking-style waits on a nested active scheduler);
    one request at a time.
  - Probe 3.2 → *Net Helper: dlaždice přímo vs přes helper* compares it with direct requests.
- **0.3:** Exit stops the network thread cleanly (it closes its connections itself; killed only
  if it hangs for 3 s). 0.2 killed it, possibly in the middle of socket/TLS calls; the phone restarted after Exit (likely cause).
- **0.4:** reads the **Bluetooth GPS natively** in its own thread (SDP search for the serial port
  service, RFCOMM, reconnects; lets Bluetooth go after a minute without requests).
  `GET /gps?addr=<12 hex digits>` answers `state= info= age= sentences= channel= connects=` lines and
  the latest `$..GGA` / `$..RMC` sentences; `/gps?stop=1` lets the GPS go. Java reading Bluetooth while
  downloading slowed every download to seconds and crashed jes-java-comms (E32USER-CBase 40, Probe 3.5).
- **0.5:** says why the GPS can't connect: a Bluetooth chip error while reaching the phone (seen:
  -6031) usually means the 9300 has a Bluetooth link to a PC (PC Suite), which blocks other links:
  state `blocked` with that hint, retried every 15 s. Also "phone doesn't answer" (-6004) and "no GPS
  sharing offered".
- **0.6:** `/gps` also on **127.0.0.1:8124**, answered by the GPS thread at once (on 8123 a GPS
  request waited behind tile downloads: up to 54 s in Probe 3.6). The GPS tries every serial port
  channel the phone offers (an Android offered 6, which stayed silent, and 11, which sent NMEA) and
  remembers the one that sends data. The screen wraps long lines and shows the GPS state; F = full screen.
- Mapy 4.4+ downloads through it when it's running; Mapy 4.15+ also takes the GPS from it.

## Install

`bin/nethelper.sis` (unsigned; Symbian 7.0s installs it after a warning), e.g. through the kit's
OTA server (`http://192.168.137.1:8000/`). Start **Net Helper** from the Desk; it must be running
while the Java app uses it.

## Build (Linux)

```sh
./build.sh    # needs ~/sym from the main repo's setup_eka1_toolchain.sh + setup_s80_sdk.sh
```

UID `0x0F5A9300` (test range 0x01000000–0x0FFFFFFF).
