# AGENTS.md

AmiRenderSwarm = cooperative render farm for AmigaOS. Two independent pieces:

- `hub/` — Go 1.26 rendezvous/signaling server (module `github.com/Afloresc/AmiRenderSwarm/hub`, module root is `hub/`). **Never** run `go` commands from the repo root; it is not a Go module.
- `amiga/` — single C agent (`bsdsocket.library`) that acts as worker and/or director.

Hub is signaling-only: scene/frame bytes go peer-to-peer over a direct TCP connection with a 16-byte big-endian `AMIR` header.

## Build & verify

Hub (run from `hub/`):

```
go build ./...        # produces hub.exe in hub/ — NOT gitignored, delete it afterwards
go vet ./...          # currently clean
go test ./...         # "[no test files]" — the repo has no automated tests
go run . -addr :7900 -timeout 60s
```

Amiga agent (cross-compile; no native `m68k-amigaos-gcc` on this machine, and the Makefile uses `rm`/`%.o` so it must run in a POSIX shell):

```
# from the repo root
docker run --rm -v "${PWD}:/work" -w /work/amiga amigadev/crosstools:m68k-amigaos make clean all
```

- Image `amigadev/crosstools:m68k-amigaos` (gcc 6.5.0b) is the known-good toolchain; it is already pulled locally. Docker Desktop must be running (the daemon stops intermittently on this machine).
- `make` must be invoked **inside `amiga/`** (relative source paths).
- The build prints one pre-existing warning: `worker.c` incompatible pointer type in `accept()` (line number shifts as the file grows). It is expected; not a regression.
- `-noixemul` in `CFLAGS`/`LDFLAGS` is mandatory for this toolchain. The resulting binary still needs `bsdsocket.library` (Roadshow/AmiTCP) and `dos.library` on the Amiga — and nothing else.
- `CFLAGS` targets `-m68020`, i.e. baseline is 68020+ even though README mentions 68060/Vampire 68080.

Gotchas that will waste your time if you don't know them:

- **`.o` files are tracked in git** (`amiga/*.o` are committed). `make clean` therefore shows tracked files as deleted, and a fresh Docker build reproduces them byte-identically. Don't "fix" the diff by re-adding them.
- **`.gitignore` only contains `AmiRenderSwarm`**, so `hub/hub.exe` (from `go build ./...`) shows up as untracked.
- `gofmt -l .` flags *all* Go files, and `gofmt -d` shows a whole-file diff — the only difference is CRLF vs LF (`core.autocrlf=true` checkout, LF blobs in git). The Go sources are already gofmt-clean. **Do not run `gofmt -w` on them**: it produces an "M" file with an empty `git diff` and confusing LF/CRLF warnings.
- **Don't use `fopen()`/`FILE*` in the agent.** It drags libnix's stdio file machinery into the binary, which adds a hard `Nuutility.library` runtime dependency (and `Nudos.library`) on the Amiga. Use the DOS API (`Open`/`Seek`/`Write`/`Close` from `<proto/dos.h>`) instead — `dos.library` is already required. Verified by A/B-linking: `printf`/`snprintf`/`vsnprintf`/`fflush`/`setvbuf` are safe; `fopen` is not.
- **Don't trust `strings`-style scans for library dependencies.** The `rts` opcode is `0x4e 0x75` = ASCII `"Nu"`, so a `"dos.library"` literal can look like `"Nudos.library"`. Verify with a NUL-anchored scan (`b"\x00Nudos.library\x00"`) or by disassembling the object.

## Manual verification without an Amiga

The hub speaks line-based ASCII (`VERB key=val`, `\n`-terminated) so it can be exercised with any TCP client. Verified recipe (hub on `127.0.0.1:7900`):

```
HELLO id=smoke os=3.2 cpu=68080 fpu=1 ram_mb=512 engines=lwsn role=both listen_port=7800
PING state=IDLE load=0.1
LIST_PEERS engine=lwsn min_ram=64
```

Expected replies: `WELCOME peer_ip=... session=...`, `PONG ts=...`, then `PEER ...` lines and `PEERS_END count=N`. A PowerShell `TcpClient` + `StreamWriter`/`StreamReader` is enough; set a read timeout or the script hangs on close.

## Agent runtime

Launch on the Amiga (positional args, from `amiga/main.c`):

```
AmiRenderSwarm <hub_ip> <hub_port> <node_id> <listen_port> [render_cmd_printf]
                [--log-level <debug|info|warn|error>] [--log-file <ruta>]
```

The optional 5th argument is a **printf template** expanded as `snprintf(cmd, template, frame, scene_path, out_path)` — in that order (`%lu` frame, then `%s` scene, then `%s` out). With no template the worker runs `Copy "<scene>" "<out>"` to validate the binary transport before lwsn/povray are plugged in.

`--log-level` / `--log-file` are parsed out of `argv` before the positional args, so they may appear anywhere and accept both `--key value` and `--key=value`. Default level is `debug` (everything). `--log-file` appends to the existing file.

Logging lives in `amiga/log.c` + `amiga/log.h` (`LOG_DEBUG`/`LOG_INFO`/`LOG_WARN`/`LOG_ERROR` macros with a component tag: `MAIN`, `NET`, `PROTO`, `WORKER`, `DIR`, `HUB`). Line format:

```
2026-11-27 10:35:12.340 [T+000003s] [INFO ] [NET] Conectado al Hub en 192.168.18.26:7900 (fd=3)
```

The timestamp comes from the **real** `dos.library` `DateStamp()` (called via LVO -192 on a library opened with `OpenLibrary("dos.library")`), **not** from the toolchain's `DOSBase` (which points at libnix's Nudos). `ds_Days` is days since 1978-01-01, so add 2922 to get Unix days, then convert with the `civil_from_days` algorithm (C truncation-toward-zero division — validate against `datetime` if you touch it). `TICKS_PER_SECOND` is 50.

Runtime facts that are easy to get wrong:

- The agent hardcodes `os=3.2 cpu=68080 fpu=1 ram_mb=256 engines=lwsn,povray role=both` in its `HELLO`; only host/port/id/listen_port/render_cmd are configurable.
- Worker temp files live on `T:` (`amirs_scene_<frame>.dat`, `amirs_out_<frame>.dat`) and are `DeleteFile()`d after each frame; the director saves received frames to `T:amirs_recv_<frame>.dat`. A writable `T:` volume must exist.
- The worker accepts **one** director connection at a time, and the whole director session is handled **synchronously inside the hub line handler**. During a render the agent sends no `PING`s, so a render longer than the hub's `-timeout` (default 60s) gets the agent swept as stale. Shorten the render or raise `-timeout` when testing long jobs.
- `INVITE_REQ` is accepted unconditionally: the agent always replies `INVITE_OK` (job id defaults to `job_01`) — there is no user confirmation prompt.
- Hub-side `LIST_PEERS` excludes the requester and only returns `IDLE` peers whose role is `worker`/`both`, engine matches, and `ram_mb >= min_ram`.
- The hub never sees scene/frame payloads; those go over the direct peer connection only.

## Protocol / code gotchas

- Signaling lines are capped at 4 KB by the hub (`scanner.Buffer(..., 4096)`), but the Amiga client's receive buffer is only 512 bytes (`HUB_LINE_BUF_SIZE`), and `Hub_ReadLine` silently truncates anything longer. Keep hub replies under 512 bytes.
- Reconnect with the same node id closes the older connection; `Unregister` compares session ids so the stale handler cannot delete the new agent.
- `INVITE` does not reserve the target — the worker only becomes `BUSY` when it sends `INVITE_OK`.
- Binary header layout (16 bytes, big-endian): `magic "AMIR"` (4), `type` (1), `flags` (1), `reserved` (2), `frame` (4), `payload_len` (4). Types: `JOB_INIT=1`, `SCENE_CHUNK=2`, `RENDER_DONE=3`, `FRAME_DATA=4`, `ERR=5`. Worker caps payloads at 16 MB (`MAX_PAYLOAD_BYTES`).
- The README describes a pull-based frame queue, NAT relay and `SetTaskPri` priority handling — **not implemented yet**. Current director path sends a single hardcoded sample scene for frame 1. Trust the code over the README.

## Conventions

- README.md, `actual_state.md` and all code comments/strings are in Spanish; user-facing output is Spanish. Keep new code consistent with that.
- `actual_state.md` is an untracked scratch file with the current manual-test setup (LAN IPs, next step). Treat it as stale notes, not a spec.
- Work on a feature branch and open a PR; commit messages follow Conventional Commits (`feat:`, `refactor:`, `doc:`).
