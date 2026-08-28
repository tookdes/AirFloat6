# AirFloat6

Dedicated iOS 6 maintenance branch of AirFloat.

Goals:
- Keep compatibility with iOS 6 and 32-bit armv7 devices.
- Prefer stability, protocol correctness, and long-running reliability over new platform features.
- Avoid Swift, iOS 8+, ARC-wide migrations, and modern Xcode project rewrites.

Base:
- `trenskow/AirFloat` commit `92f26b5` (2016-06-25 background stability refactor).
- Deployment target remains iOS 6.0 and the device architecture remains armv7.

Integrated stability work includes:
- Correct POSIX timed-wait deadline normalization.
- Safer web server, socket, RAOP session, and DACP teardown/lifetime handling.
- Bounded RTSP, authentication, metadata, Base64, and ALAC format parsing.
- Improved missing-packet search and retransmit retry handling.
- Correct 16-bit RTP sequence and 32-bit RTP timestamp wrap handling.
- Persistent RTP timing synchronization instead of stopping after the first periodic response.
- RTSP `GET_PARAMETER` keep-alive support for classic AirPlay senders.
- Variable RTP packet timing tolerated by Debug queue consistency checks, avoiding the known Apple TV Debug-build `SIGABRT`.
- Normal iOS builds use the system AudioConverter ALAC backend; the legacy software ALAC decoder is retained only by the explicit `Debug (Server Logs - ALAC Software)` diagnostic configuration.
- Classic AirPlay ALAC negotiation is bounded to at most 16384 frames per packet and mono/stereo audio.
- Blurred artwork releases its CoreGraphics backing image instead of leaking one image per artwork update.
- Repeated foreground/background cycles keep a reusable helper background-task expiration handler.

Known protocol note:
- The existing RAOP Bonjour TXT record still contains a duplicated `sr` key and does not advertise the password-required `pw` flag. This is intentionally left for a focused Bonjour change rather than rewriting the large Zeroconf implementation as part of unrelated fixes.

Recommended iOS 6 regression checks:
- Connect, play, pause for more than 60 seconds, resume, then reconnect without restarting AirFloat.
- Exercise FLUSH/seek around RTP sequence wrap and play long enough to cross a 32-bit RTP timestamp boundary when practical.
- Disconnect Wi-Fi or kill the sender during playback, then verify AirFloat becomes available again without an app restart.
- Repeat connect/disconnect cycles and server stop/start cycles.
- Stream from an Apple TV using a Debug build and confirm variable packet framing no longer aborts the queue consistency check.
- Change tracks/artwork repeatedly and watch memory use for steady-state behavior.
- Build both Debug and Release; normal iOS builds should use AudioConverter ALAC, while the dedicated ALAC Software debug configuration should remain available for comparison.

Excluded from this branch:
- iOS 8+ deployment target changes.
- Xcode 8/9 project migrations.
- ARC conversion.
- Newer Swift-based rewrites.
