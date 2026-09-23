# TV VNC dependency changes

Modified 2026-09-21 and 2026-09-23, based on LibVNCServer commit
`42494999e6492aaab9c1db785ecd293ef10b3aed`. Original copyright and
GPL-2.0-or-later licensing remain unchanged.

The client reader distinguishes buffered message readiness from socket
readiness. Partial buffered reads no longer burn the timeout without waiting.
Both read paths charge their existing cumulative idle budget by monotonic
elapsed socket-wait time, not the number of fragments. Read progress does not
reset the budget; copying and processing ready bytes do not consume it.
Zero timeout remains unlimited. Interrupted read waits report no data yet,
without resetting the idle budget; interrupted write waits resume. Other clock/socket errors return failure,
never terminate the host process. CLOCK_MONOTONIC excludes suspend time on
Linux/Android and includes it on macOS; the application owns background teardown.

The added socket test uses explicit checks that remain active in release
builds. It covers short and large fragmented reads, cumulative idle expiry,
zero-timeout behavior, buffered/replay readiness, immediate EOF, invalid
descriptors, interrupted waits/reads without extending the idle budget, and
interrupted writes against a full socket send buffer.
Current runtime validation is macOS and Android without TLS/SASL. No Windows,
TLS, or SASL runtime validation is claimed.

The maintained fork is https://github.com/amersheeny/tvvnc-libvncserver.
Applications must pin a published commit from that fork rather than a local
filesystem URL. These modifications have not been submitted upstream.
