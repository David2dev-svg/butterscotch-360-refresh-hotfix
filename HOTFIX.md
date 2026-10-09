# Butterscotch360-Refresh hotfix (Xbox 360, OXDK build)

Fixes and a new build system on top of flaf1x/Butterscotch360-Refresh (archived).
Original history and license are kept. Credit to the Butterscotch project and to
flaf1x for the Xbox 360 refresh.

## What changed
- Own build with OXDK (patched LLVM, `XENON_ABI=1`), no MSVC needed
- File system vtable completed and path resolution fixed
- Audio groups, streams and music fixed
- Texture decoder for BZip2+QOI (Deltarune)
- ds_map_delete and ds_map_keys_to_array
- main.cpp: SEH blocks guarded with `#ifdef __clang__` (MSVC build is unchanged)

## Build (you provide your own XDK and OXDK; none is included here)
1. Patched clang from OXDK with the xenon-abi feature.
2. XDK headers on a case-sensitive filesystem may need links, for example:
   `ln -s xobjbase.h <XDK>/include/xbox/XObjBase.h`
3. Edit OXDK_DIR in `oxdk/Makefile.full` if OXDK is not in `$HOME/OXDK`, then:

        cd oxdk
        python3 gen_srcs.py
        make XENON_ABI=1 -f Makefile.full

Put `data.win` next to `default.xex` (`game:\data.win`).
The sources list is generated from `Butterscotch360.vcxproj`; `.c` files are
built as C++ like the original MSVC project.
