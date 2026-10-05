# Net Helper 9300

A native (Symbian C++) helper for the Java apps on the **Nokia 9300** (Series 80 2.0, Symbian 7.0s EKA1).

Java MIDlets on the 9300 can't keep connections open: every `HttpConnection` is a new TCP connection
and, for HTTPS, a new TLS handshake (~0.5–1 s extra per request, measured), and unsigned MIDlets may
not open `socket://` to ports 80/443. They may, however, talk to `http://127.0.0.1`. The helper runs
on the phone, listens there and will do the network work for them over kept-open connections.

## Status

- **Step 1 (0.1):** the app listens on `127.0.0.1:8123`, answers every HTTP request with a short
  text and shows how many requests it got. Probe 2.9 → *Test pomocníka* checks that a MIDlet can
  reach it.
- Step 2: forward the Java apps' requests over kept-open HTTP/HTTPS connections (keep-alive; TLS
  through the phone's patched `SSLADAPTOR.dll`).

## Install

`bin/nethelper.sis` (unsigned; Symbian 7.0s installs it after a warning), e.g. through the kit's
OTA server (`http://192.168.137.1:8000/`). Start **Net Helper** from the Desk; it must be running
while the Java app uses it.

## Build (Linux)

```sh
./build.sh    # needs ~/sym from the main repo's setup_eka1_toolchain.sh + setup_s80_sdk.sh
```

UID `0x0F5A9300` (test range 0x01000000–0x0FFFFFFF).
