# AirFloat6

Dedicated iOS 6 maintenance branch of AirFloat.

Goals:
- Keep compatibility with iOS 6 devices.
- Prefer stability fixes over new system features.
- Avoid Swift, iOS 8+, ARC-wide migrations, and modern Xcode project rewrites.

Base:
- trenskow/AirFloat commit 92f26b5 (background stability refactor).

Integrated fixes:
- condition timed wait deadline calculation fix.

Excluded from this branch:
- iOS 8+ deployment target changes.
- Xcode 8/9 project migrations.
- ARC conversion.
- Newer Swift based rewrites.
