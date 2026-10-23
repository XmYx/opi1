# third_party/

Vendored libraries, one directory each with its own `CMakeLists.txt` (the top-level build adds every
`third_party/*/CMakeLists.txt` automatically). A module links one through its `src/<module>/module.cmake`.

For a 1:1 port, use the versions the OG firmware was built with. The firmware's own assert strings name
them (R.00246, `strings` of the VDK image, see `docs/firmware-behaviour.md` §2):

| library | version in the firmware | evidence | used by | owner | licence |
|---|---|---|---|---|---|
| Box2D | 2.0.x (`Source/` layout, sweep-and-prune broadphase, `b2_maxProxies`, `m_world->m_lock`) | `..\ext\Box2D\Source\Collision\b2BroadPhase.cpp:70` and 36 more asserts | tombola sequencer, lander | t5 | zlib |
| jsoncpp | 0.5.0 | `..\ext\jsoncpp-src-0.5.0\src\lib_json\json_reader.cpp:410` | patch and preset JSON | t5 | public domain / MIT |
| SQLite | 3.6.20 (`sqlite3-bf`) | version string; 2,100+ `..\ext\sqlite3-bf` asserts | op1.db, op1_factory.db, tape.db | t5 (any 3.x reads these files; the system library is fine) | public domain |
| libaiff | 5.0 | `..\ext\libaiff-5.0\aiff_file.c:32` | tape and album AIFF files | t5 | see its licence |
| AGG (Anti-Grain Geometry) | 2.x, to be pinned from the binary | screen rendering (t1 §2) | screen rasteriser | t6 | 2.4 is permissive; 2.5+ is GPL: use 2.4 |
| rapidxml | to be pinned | SVG screen parsing (t1 §2) | screen loader | t6 | Boost / MIT |

Rules: pin the exact version, keep the upstream licence file next to the source, note any local patch
in that directory's README, and do not vendor GPL code (the port is not licensed under the GPL).
Nothing from Teenage Engineering goes here: screens, fonts and the factory database are read from the
user's firmware image at run time (`include/op1/ui.h`, `AssetProvider`).
