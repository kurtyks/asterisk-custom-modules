# Build integration guide — cdr_redis

This document describes every change required to integrate `cdr_redis` into the
Asterisk 23 source tree so that it is detected by the build system, compiled,
and linked correctly.

---

## Files added or modified

### Added

| File | Purpose |
|---|---|
| `cdr/cdr_redis.c` | Module source |
| `configs/samples/cdr_redis.conf.sample` | Sample configuration |

### Modified

| File | Change |
|---|---|
| `configure.ac` | Register and detect the `hiredis` external library |
| `build_tools/menuselect-deps.in` | Expose `HIREDIS` detection result to menuselect |
| `makeopts.in` | Expose `HIREDIS_LIB` and `HIREDIS_INCLUDE` to the module Makefiles |

---

## How Asterisk's external library system works

Asterisk uses a three-layer system to manage optional external dependencies:

### Layer 1 — `configure.ac`

Two macros from `autoconf/ast_ext_lib.m4` are used for each library:

```m4
AST_EXT_LIB_SETUP([NAME], [description], [option-name])
AST_EXT_LIB_CHECK([NAME], [lib], [function], [header])
```

`SETUP` registers the library with the build system and creates the
`--with-NAME=DIR` configure flag. `CHECK` calls `AC_CHECK_LIB` and
`AC_CHECK_HEADER`; on success it sets:

- `PBX_NAME=1` — library is available
- `NAME_LIB=-lname` — linker flag passed to module compilation
- `NAME_INCLUDE=` — compiler flag (include path, if needed)

For `hiredis`:

```m4
# near the BEANSTALK entry (~line 600)
AST_EXT_LIB_SETUP([HIREDIS], [hiredis Redis client], [hiredis])

# near the BEANSTALK check (~line 2449)
AST_EXT_LIB_CHECK([HIREDIS], [hiredis], [redisConnect], [hiredis/hiredis.h])
```

On Fedora/RHEL, hiredis installs headers under `/usr/include/hiredis/`, so the
header argument must be `hiredis/hiredis.h` (not `hiredis.h`).

### Layer 2 — `build_tools/menuselect-deps.in`

`configure` generates `build_tools/menuselect-deps` from this template via
`AC_CONFIG_FILES`. The file is what the `menuselect` tool actually reads to
determine whether a module's dependencies are satisfied. Without an entry here,
menuselect never sees the library — even if `PBX_HIREDIS=1` is set in `makeopts`.

Add one line, alphabetically near `BEANSTALK`:

```
HIREDIS=@PBX_HIREDIS@
```

### Layer 3 — `MODULEINFO` block in the C source

The `<depend>` tag inside `MODULEINFO` ties the module to the library name
registered in `configure.ac`. The name must be lowercase and match the third
argument of `AST_EXT_LIB_SETUP`:

```c
/*** MODULEINFO
    <depend>hiredis</depend>
    <support_level>extended</support_level>
 ***/
```

When menuselect reads the module, it maps `hiredis` → `HIREDIS`, checks
`build_tools/menuselect-deps` for `HIREDIS=1`, and marks the module as
buildable. The build system then reads `HIREDIS_LIB` from `makeopts` and appends
`-lhiredis` to the linker command for `cdr_redis.so`.

---

## What happens without these changes

| Missing | Symptom |
|---|---|
| `AST_EXT_LIB_SETUP` / `AST_EXT_LIB_CHECK` in `configure.ac` | `PBX_HIREDIS` never set; `HIREDIS_LIB` empty |
| `HIREDIS=@PBX_HIREDIS@` in `menuselect-deps.in` | menuselect marks cdr_redis as "unmet dependency"; module excluded from build even if `--enable cdr_redis` is called |
| `HIREDIS_LIB=-lhiredis` in `makeopts` | Module compiles but fails to link: `undefined symbol: redisFree` |

The third failure mode is the most subtle: `make` silently skips the module on
link error when building in parallel (`-j`), so nothing in the standard build
output indicates the problem.

---

## Build system data flow

```
configure.ac
    └─ AST_EXT_LIB_CHECK([HIREDIS], ...)
           │
           ├──► makeopts          (PBX_HIREDIS=1, HIREDIS_LIB=-lhiredis)
           │
           └──► build_tools/menuselect-deps   (HIREDIS=1)
                      │
                      └──► menuselect --check-deps
                                 │
                                 └──► menuselect.makeopts  (cdr_redis enabled)
                                            │
                                            └──► make
                                                   │
                                                   ├── gcc ... cdr_redis.c
                                                   └── ld  ... -lhiredis
```

---

## Enabling the module in menuselect

After configure runs, enable `cdr_redis` explicitly before building:

```bash
make menuselect.makeopts
menuselect/menuselect --enable cdr_redis menuselect.makeopts
make -j"$(nproc)"
make install
```

Or interactively:

```bash
make menuselect
# Navigate to Call Detail Recording → cdr_redis
```

---

## Package dependencies

| Distribution | Package | Provides |
|---|---|---|
| Fedora / RHEL | `hiredis-devel` | `/usr/include/hiredis/hiredis.h`, `/usr/lib64/libhiredis.so` |
| Debian / Ubuntu | `libhiredis-dev` | `/usr/include/hiredis/hiredis.h`, `/usr/lib/libhiredis.so` |

The `#include` in the source uses the full path:

```c
#include <hiredis/hiredis.h>
```

This works on both distributions without additional `-I` flags since both install
under `/usr/include/`.

---

## Notes on regenerating `configure`

The Asterisk source tree ships a pre-generated `configure` script. After
modifying `configure.ac` you must regenerate it:

```bash
./bootstrap.sh   # runs aclocal, autoconf, autoheader, automake
```

Or directly:

```bash
autoreconf -fi
```

The `bootstrap.sh` script requires `autoconf`, `automake`, and `aclocal`. On
Fedora these are provided by the `autoconf` and `automake` packages.

If `configure` is not regenerated, the `AST_EXT_LIB_CHECK` changes are silently
ignored and `PBX_HIREDIS` is never set. The fallback documented in the Docker
section below patches the generated files directly to work around this.

---

## Docker build notes

The `Dockerfile` applies two safety patches after `./configure` runs, in case
`bootstrap.sh` did not fully regenerate `configure` from the modified
`configure.ac`:

```dockerfile
# Ensure menuselect sees hiredis as available
&& sed -i '/^HIREDIS=/d' build_tools/menuselect-deps \
&& echo 'HIREDIS=1' >> build_tools/menuselect-deps \

# Ensure the linker flag is present for module compilation
&& sed -i '/^HIREDIS_LIB=/d; /^HIREDIS_INCLUDE=/d' makeopts \
&& printf 'HIREDIS_LIB=-lhiredis\nHIREDIS_INCLUDE=\n' >> makeopts \
```

These patches are belt-and-suspenders: with the `configure.ac` and
`menuselect-deps.in` changes in place they are redundant, but they make the
Docker build robust against autoconf toolchain version mismatches on the build
host.

---

## What could have been simpler

Per [Asterisk Build System Architecture](https://docs.asterisk.org/Development/Reference-Information/Other-Reference-Information/Build-System-Architecture/)
and [Coding Guidelines](https://docs.asterisk.org/Development/Policies-and-Procedures/Coding-Guidelines/):

| Our approach | Correct approach per docs |
|---|---|
| Missing `makeopts.in` → manual Dockerfile patch for `HIREDIS_LIB` | Add `HIREDIS_LIB=@HIREDIS_LIB@` and `HIREDIS_INCLUDE=@HIREDIS_INCLUDE@` to `makeopts.in` — this is the third required file for any module with dependencies |
| `atoi()` for numeric config values | `sscanf()` with return value check (per coding guidelines: "use `sscanf()` rather than `atoi()`") — now fixed |
| `ast_rwlock_rdlock` held during network I/O | Snapshot config under lock, release before I/O — safer under high reload frequency |
| Per-CDR TCP connection | Persistent connection with reconnect-on-error, matching `cdr_pgsql.c` pattern |

The `makeopts.in` omission was the root cause of the `undefined symbol: redisFree`
error and all the subsequent Dockerfile workarounds. The docs describe it as one
of three mandatory file changes for any module with external dependencies.
