# TLS Certificates for api.airgradient.com

## Current chain (observed 2026-11)

```text
0 CN=api.airgradient.com          issued by  Let's Encrypt YR2
1 Let's Encrypt YR2               issued by  ISRG Root YR
2 ISRG Root YR (cross-signed)     issued by  ISRG Root X1
```

Leaf certificates rotate about every 60–90 days, so **never pin the leaf or
an intermediate**. Embed long-lived roots only.

## Strategy

| Option | Pros | Cons |
| :--- | :--- | :--- |
| **Embed ISRG Root X1 + X2 (default)** | Small (~3 KB), valid until 2035+ | Breaks if AirGradient changes CA vendor, or LE stops serving the X1 cross-sign |
| Arduino-ESP32 CA bundle (`client.setCACertBundle(...)`) | Covers all Mozilla roots | ~60 KB flash, setup differs between core 2.x and 3.x |
| `setInsecure()` | Always works | No server authentication. **Debug env only** |

If `make_ca_header.sh` prints the WARNING line, the served chain no longer
ends at X1 or X2. Download the new root named in the last `i:` line (from
https://letsencrypt.org/certificates/ or the CA's site), append it to the
bundle, regenerate, and reflash.

## Error codes (mbedTLS)

| Code | Meaning | Usual cause |
| :--- | :--- | :--- |
| `-0x2700` | X509 cert verify failed | Wrong CA, or clock not set |
| `-0x7780` | Fatal alert from peer | SNI or protocol mismatch |
| `-0x0050` / `-0x004C` | Net send/recv failed | WiFi dropped mid-handshake |
| `-0x7200` | Invalid record | Proxy or captive portal |
| `-0x2880` | Bad input data | Malformed PEM (check the `\n` at each line end) |

## Heap

A handshake with an RSA chain needs about 35–45 KB of free heap. Before each
fetch, log `ESP.getFreeHeap()`. If it's under 60 KB, skip the cycle and warn
rather than crash.
