# deps/

HydroCore needs two things built here before you can compile it:

- `luajit/` from https://github.com/LuaJIT/LuaJIT, built static.
  `cd luajit/src && msvcbuild.bat static` produces `lua51.lib`, which CMake
  picks up automatically.

- `ue4ss/` from https://github.com/UE4SS-RE/RE-UE4SS, experimental `main` for
  UE 5.5. Needs its own Rust + CMake + Ninja setup; see upstream docs. Has to
  be built at least once before HydroCore will link.

Neither is vendored in this repo.
