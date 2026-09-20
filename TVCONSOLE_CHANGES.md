# Local TV Console dependency changes

Modified 2026-09-21, based on LibVNCServer commit
`42494999e6492aaab9c1db785ecd293ef10b3aed`. Original copyright and
GPL-2.0-or-later licensing remain unchanged.

The client reader distinguishes buffered message readiness from socket
readiness. Partial buffered reads no longer burn the timeout without waiting.
Both read paths charge their existing cumulative idle budget by monotonic
elapsed socket-wait time, not the number of fragments. Read progress does not
reset the budget; copying and processing ready bytes do not consume it.
Zero timeout remains unlimited. Clock/socket errors return failure, never
terminate the host process. POSIX CLOCK_MONOTONIC excludes suspend time; the
application owns background teardown.

The added socket test uses explicit checks that remain active in release
builds. It covers short and large fragmented reads, cumulative idle expiry,
zero-timeout behavior, buffered/replay readiness, EOF, and invalid descriptors.
Current runtime validation is macOS and Android without TLS/SASL. No Windows,
TLS, or SASL runtime validation is claimed.

This is a local development fork. No upstream submission or public publication
has been authorized. A distributable app must use a fetchable fork URL before
publication; a local filesystem URL is not a public release dependency.
