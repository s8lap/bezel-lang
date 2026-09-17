# Proposal 0071 — File metadata: `FileStatus` snapshot record + `File.GetStatus` family

| | |
|---|---|
| **Status** | under-review — draft |
| **Tracking issue** | — (none yet) |
| **Target module(s)** | `System.IO` (extends stdlib §7.3 `File`; new `FileStatus`/`FileKind` types in a new §7.3b — numbering at docs-editor discretion, the 0009 precedent; 0017 already claims §7.3a) |
| **Written against** | spec/stdlib **v0.227** |
| **Gap-review item** | — (not among the [gap review](stdlib-gap-review-2026-07.md)'s 31 items; see §0) |
| **Currency note** | Refreshed 2026-08-11 to v0.246: §17.11's zero-constant diagnostic is an error since v0.242 (was a warning) — the §8 claim conformed; the design is unaffected (`FileKind` has the zero constant). Written-against pin unchanged. **2026-09-17 sweep — refreshed to v0.306:** `DateTime` ordering respelled to `CompareTo` at D3's instant-idiom sentence and in §9's `NeedsRebuild` example (`src.ModifiedAt > built.ModifiedAt` → `src.ModifiedAt.CompareTo(built.ModifiedAt) > 0`) — stdlib §2.5 deleted the claimed `<`/`>`/`<=`/`>=` operator group at v0.274 and now states *"Ordering is CompareTo only"*; tick-order semantics and the design are unchanged. No tier-2 flag. Written-against pin unchanged. |

---

## 0. Evidence and position on the gap map

Stdlib §7.3 `File` declares exactly: `ReadAllText`, `WriteAllText`, `ReadAllBytes`, `WriteAllBytes` (×2 overloads), `ReadAllLines`, `WriteAllLines`, `ReadAllTextAsync`, `WriteAllTextAsync`, `Exists`, `Delete`, `Copy`, `Move`. §7.4's `Directory`/`Path` add existence, creation, deletion, listing, and path string operations. **There is no metadata surface anywhere in §7**: no way to read a file's size, its modification time, or its kind (file / directory / symlink). Today:

- a file's **size** is reachable only by *opening* the file (`new FileStream(path, FileMode.Open)` + the inherited `Stream.Length`, §7.1/§7.2) — a handle acquisition with permission and sharing costs, absurd inside a directory walk — or by reading the entire contents;
- a file's **modification time** is entirely unreachable;
- **existence-with-kind** requires two racing calls (`File.Exists` + `Directory.Exists`), with no symlink story at all.

The [2026-07 stdlib gap review](stdlib-gap-review-2026-07.md)'s io-fs items are **#1** (concrete stream family), **#22** (async File family), and **#25** (temp files + Path/Directory utilities) — none includes stat: #25, the closest, is path *manipulation* and temp-file *lifecycle* (its designed form, proposal 0017 §1, lists "what is missing" and metadata is not on the list). The nine-domain survey simply missed this gap; a subsequent adversarial review pass confirmed it. Services and tools need it constantly — cache invalidation, log rotation, directory walking with sizes (§2). This proposal extends the gap map.

## 1. What exists today (the substrate this builds on)

All items verified in the current docs; nothing invented:

- **`File`** (§7.3) — the static-convenience home the new members extend; `Exists`, `Delete`, `Copy`, `Move` establish the path-string, synchronous, `§7.5`-faulting style.
- **`FileStream`** (§7.2) — `Length` via `Stream` (§7.1): the only size today, behind an open handle.
- **`DateTime`** (§2.5) — value record with `Kind: DateTimeKind` (`Utc` among them); `FromUnixTimeSeconds` produces `Kind = Utc`; `Ticks: long` at the 100 ns tick the §2.18 `c` vocabulary's seven fractional digits encode; full comparison-operator group ("tick order"). This is the mtime type (D3).
- **IO exceptions** (§7.5) — `IOException`, `FileNotFoundException`, `DirectoryNotFoundException`, `PathTooLongException`; all derive from `IOException`; `import System.IO` required.
- **The Try convention** (§1) — shape (c), `bool` + non-nullable value-typed out with `default(T)` on the false path, is the sanctioned shape for concrete value-type payloads (`DateTime.TryParse`, `Guid.TryParse`, `Convert.TryTo*`).
- **Replay intercept points** (§1, v0.204) — §7 filesystem reads are intercept points; a new world-reading member must declare itself one in its own section.
- **Proposal 0017** (temp files + Path/Directory completions) — the adjacent io-fs proposal this one must not overlap (§6).
- **Proposal 0018** (async File family) — the async-duals convention, including the recorded rule that metadata operations get **no** async dual (0018 §7).

## 2. Motivation: concrete workloads

1. **Cache invalidation / build tools.** "Rebuild iff source mtime > output mtime" is the first line of every incremental tool; proposal 0040's `bezel build` scan phase wants exactly this cheap early-out before content hashing. Today it is unwritable.
2. **Log rotation.** "Rotate when the log exceeds N bytes" (the sink side of proposal 0021's logging) needs size-without-open; today the logger would have to open its own log a second time or track bytes out-of-band.
3. **Directory walking with sizes.** Disk-usage tools, quota enforcement, artifact sweepers (`gc_scratch`-style tools) walk `Directory.GetFiles` output and need per-entry kind and size — one stat per entry, no handle churn.
4. **Static file serving.** 0002's deferred static-file lane needs `Content-Length` before streaming and a cheap validator (ETag from size + mtime).
5. **Sync/deploy tools.** "Same size and mtime ⇒ skip" comparisons; also the symlink-safety check ("do not follow links out of the release tree"), which requires the *no-follow* stat form (D4).
6. **Existence-with-kind.** "Is this path a directory, a file, or gone?" as one race-free question instead of two racing booleans.

## 3. D1/D2/D3 — Shape, staleness, and the mtime type

### D1 — A snapshot value record, not static per-fact methods, not a live `FileInfo` class

**Decision.** One stat call returns one **value record**:

```bezel
public record FileStatus(Kind: FileKind, Size: long, ModifiedAt: DateTime, IsReadOnly: bool);
```

**Against .NET's per-fact statics** (`File.GetLastWriteTimeUtc(path)`, `GetAttributes(path)`, …): N facts would cost N syscalls and — worse — N *separately-timed* reads, so "size and mtime" could come from two different versions of the file. One record from one stat is cheaper and internally consistent.

**Against a live `FileInfo`-style class** (path object with `Refresh()`, `Delete()`, `CopyTo()`): a class drags in the ownership regime (`owned FileInfo`, `using var`) for data with nothing to dispose; .NET's cached-until-`Refresh` semantics are a recorded trap (stale reads that look live); and the path-verb methods would duplicate §7.3's existing statics. The record is data; the verbs stay on `File`. The name `FileInfo` is declined with the shape (§8#1) — reusing .NET's name for different semantics invites wrong expectations; `FileStatus` names the syscall it wraps.

### D2 — Staleness semantics: a snapshot, not a live handle

A `FileStatus` is **a snapshot of the metadata at the moment of the call** — it never updates, there is no `Refresh()`, and holding one holds nothing open. Re-stat by calling `GetStatus` again. This is stated in the record's doc text, along with the honest TOCTOU note: metadata answers "what was true then," and is for *coordination and heuristics* (rebuild checks, rotation thresholds, walk pruning), never for *authorization* — the way to know you can open a file is to open it and handle the §7.5 fault. (The race-free companion — stat of an already-open handle, fstat-style, on `FileStream` — is deferred, §8#5; note `FileStream` already exposes `Length` for the size half.)

Equality of two `FileStatus` values is the auto-generated structural equality over the four components (spec §9.1; every component is admissible — enum, `long`, the `DateTime` value record, `bool`), which is exactly the "same size and mtime" comparison the sync workload wants. The snapshot deliberately does **not** carry the queried path (§8#2): the caller has the path, and including it would make identical metadata compare unequal, breaking the dedup/skip use.

### D3 — `ModifiedAt` is a `DateTime` with `Kind = Utc`

**Decision.** `ModifiedAt: DateTime`, always `Kind = Utc`, truncated to `DateTime`'s 100 ns tick.

The alternatives, each checked against its source:

- **0016's `Instant`** (monotonic time; draft) is explicitly *not* wall-clock — it has "no conversion to `DateTime`" and answers only elapsed-time questions. An mtime is a wall-clock fact recorded by the filesystem; `Instant` is the wrong type by 0016's own charter.
- **0012's `DateTimeOffset`** (accepted 2026-07-07, delegated; **not yet landed** in the stdlib) is instant + *civil offset* — but a filesystem timestamp carries no civil offset; the only honest offset would be a constant `+00:00`, adding a field of ceremony and, more decisively, sequencing this proposal behind 0012's landing for zero semantic gain. When 0012 lands, `DateTimeOffset.FromDateTime(st.ModifiedAt)` (well-defined for `Kind = Utc` per 0012 D2) bridges losslessly; a `ModifiedAtOffset` accessor would be a compatible widening.
- **`DateTime` with `Kind = Utc`** is the stdlib-current instant idiom: §2.5's own `FromUnixTimeSeconds` produces `Kind = Utc`, and `ToUnixTimeSeconds` round-trips it deterministically ("no hidden machine-zone read"). Comparison is `CompareTo` (tick order) — `DateTime` declares no `<`/`>`/`<=`/`>=` overloads (§2.5, v0.274) — and is correct for two `Utc` values, which is all this API emits.

**Precision pin:** platform timestamps (commonly nanoseconds) are truncated **toward negative infinity** (floor) to the 100 ns tick, so the order of two distinct platform timestamps is never inverted by conversion. (0016's `FromNanoseconds` truncates *toward zero*; the divergence is deliberate and stated — a duration's sign symmetry wants toward-zero, a timeline's ordering wants floor — and is invisible for all post-1970 timestamps, where the two rules coincide.)

`CreatedAt`/`AccessedAt` are declined for v1 (§8#3).

## 4. D4/D5 — Symlink posture and the permission surface

### D4 — Follow by default (`stat`); the no-follow form is explicit (`GetLinkStatus`)

**Decision.** `GetStatus`/`TryGetStatus` **follow** symbolic links (POSIX `stat`): a consumer asking for size/mtime almost always means the target, so `FileKind.Symlink` never appears in their results, and a *dangling* link reports absent (`FileNotFoundException` / `false`). The no-follow form (POSIX `lstat`) is the explicitly-named pair `GetLinkStatus`/`TryGetLinkStatus`: a symlink reports `Kind = Symlink`, its own mtime, and `Size` = the byte length of the link-target path (which is what the platform stores — meaningful, and pinned). On a path that is not a symlink the two families agree.

Both are shipped (not just the default) because the walker/deploy workloads (§2#3, §2#5) are impossible without lstat: distinguishing a link from its target *is* the point there. Naming: `GetLinkStatus` over a `followLinks: bool` parameter — a bool flag would make the dangerous variant a silent argument; a distinct name is greppable and self-announcing (the `Delete(path, recursive)` counter-style was considered; explicitness won because follow/no-follow changes the *meaning* of every field, not just the operation's reach).

Reading a link's target (`ReadLink`) is deferred (§8#4).

### D5 — The permission surface: exactly one bit

**Decision.** `IsReadOnly: bool` — true when the platform metadata marks the entry read-only: on POSIX, the owner-write permission bit is clear (`mode & 0o200 = 0`); on Windows, the ReadOnly attribute is set. This is the one permission fact both platform metadata models store *as data* (and the mapping .NET uses, so it round-trips with foreign tools' expectations).

Declined beyond it: full POSIX mode bits, owner/group ids, and `IsExecutable` have no Windows analogue (a cross-platform lie or a permanent platform-behavior fork — a future `System.IO` POSIX-extensions surface is the right home, §8#6); an *effective-access* check (`CanWrite` for the current user — `access(2)`) is a different, ACL-dependent, inherently racy question, and D2's posture already routes authorization to open-and-fault. `IsReadOnly` is metadata, stated as such: it does not promise a write will succeed, only that the platform flags the file read-only.

## 5. Proposed API surface

All items below are **proposed** (module `System.IO`; not auto-imported, like the rest of §7). Faults: §7's exception types (§7.5).

```bezel
/// The kind of filesystem entry a FileStatus describes.
public enum FileKind: byte
  File,        // regular file
  Directory,
  Symlink,     // only ever reported by the GetLinkStatus family (D4)
  Other;       // sockets, FIFOs, devices — present, but none of the above
end FileKind;

/// A snapshot of one filesystem entry's metadata, from a single stat-shaped
/// read (D1). A snapshot, not a live handle: it never updates and holds
/// nothing open — re-stat by calling GetStatus again (D2). Metadata is for
/// coordination, never authorization: to learn whether an open will succeed,
/// open and handle the fault.
public record FileStatus(
    Kind: FileKind,
    Size: long,             // byte length for Kind = File; the link-target path's
                            //   byte length for Kind = Symlink (lstat family);
                            //   pinned 0 for Directory and Other (platform st_size
                            //   is meaningless and platform-varying there)
    ModifiedAt: DateTime,   // Kind = Utc always; floor-truncated to the 100 ns
                            //   tick (D3)
    IsReadOnly: bool);      // platform read-only flag: POSIX owner-write bit
                            //   clear / Windows ReadOnly attribute (D5)
```

```bezel
// Extensions to File (System.IO, §7.3):
public static class File
  // ... (existing members unchanged — see §1)

  /// Reads the entry's metadata in one call, FOLLOWING symbolic links (D4).
  /// FileNotFoundException when nothing exists at `path` (including a dangling
  /// symlink and a missing path component); IOException for other failures
  /// (e.g. permission denied on a path component).
  public static method GetStatus(path: string): FileStatus;                        // PROPOSED

  /// Try form (§1 shape (c)): returns false — assigning default-valued status —
  /// exactly where GetStatus would raise FileNotFoundException. Every OTHER
  /// fault (IOException) still raises: Try means "absent is an expected
  /// outcome," not "swallow all faults."
  public static method TryGetStatus(path: string, out status: FileStatus): bool;   // PROPOSED

  /// The no-follow (lstat) forms: a trailing symbolic link reports itself —
  /// Kind = Symlink, its own ModifiedAt, Size = target-path byte length (D4).
  /// Same fault contract as GetStatus/TryGetStatus otherwise.
  public static method GetLinkStatus(path: string): FileStatus;                        // PROPOSED
  public static method TryGetLinkStatus(path: string, out status: FileStatus): bool;   // PROPOSED
end File;
```

**Intercept-point declaration (stdlib §1, v0.204).** All four members read the world and join §7's replay intercept points, declared in their section at landing: a conforming runtime may record their results and substitute them on replay.

**No `Directory.GetStatus`.** The operations are path-based and kind-discriminated: `File.GetStatus` on a directory path succeeds with `Kind = Directory`. A parallel `Directory` entry point would be a second name for the same syscall (and .NET's split here is pure legacy). Stated in the §7.3b text so nobody files the "missing" member.

**No async duals.** 0018 §7's recorded rule applies verbatim: `Exists`/`Delete`/`Copy`/`Move` got no async dual because "they are metadata or rename operations … their syscall overhead is trivial and the added complexity of a `Task` return type is not justified." A stat is the paradigm case. Anyone needing bulk-stat concurrency has the ordinary spawn machinery (and `FileStatus` is sendable, §7).

## 6. Coordination with proposal 0017 (the seam, stated)

Proposal 0017 owns the *path/temp-lifecycle* completions: `Path.ChangeExtension`/`GetTempPath`/`Combine` overloads, `Directory.GetFiles(pattern)`/`GetDirectories(pattern)`/`CreateTempSubdirectory`/`Move`, and `TempFile`. This proposal adds **no `Path` or `Directory` member and no lifecycle type** — zero overlap; 0017 §1's "what is missing" list confirms metadata was never in its scope. The two compose at the obvious seam: 0017's pattern-listing feeds 0071's per-entry stat (`Directory.GetFiles(root, "*.log")` → `File.GetStatus(f).Size`), and `TempFile.Path` is a valid `GetStatus` argument like any other path. Neither proposal depends on the other; they can land in either order.

## 7. Ownership, sendability, faults

**Ownership.** `FileStatus` is a value record: no `owned` anywhere on this surface — no disposal obligations are created by a stat (contrast `FileStream`). The `out status: FileStatus` parameter is Try shape (c) (§1): non-nullable value-typed out, `default(FileStatus)` assigned on the false path — well-defined per spec §17.11 (enum + `long` + `DateTime` + `bool` all have zero values; `FileKind`'s first constant `File` is the zero value, so the default is a real, bool-discriminated value exactly as the convention prescribes; shape (c)'s rule that the bool alone distinguishes applies verbatim).

**Sendability.** `FileStatus` holds an enum, a `long`, a `DateTime` (a value record of primitives), and a `bool` — not borrow-bearing (spec §9.7), hence **sendable** (spec §30.14): a parallel directory walker may fan stat results across domains as spawn results or channel messages, copies throughout. Stated as contract.

**Serialization** is deferred, with a named dependency: `FileStatus` cannot declare `ISerializable`/`IDeserializable` until its `DateTime` component is serialization-eligible, which is exactly proposal 0010's charter (Guid/DateTime/TimeSpan conformances). When 0010 lands, adding the conformance here is a compatible widening; declaring it now would be a §9.8 compile error at the conformance declaration.

**Fault taxonomy** (all types exist in §7.5 / spec §31.4; no new exception types):

| Condition | `GetStatus` / `GetLinkStatus` | `TryGetStatus` / `TryGetLinkStatus` |
|---|---|---|
| Nothing at `path` (final component absent; a missing/non-directory parent component; a dangling symlink in follow mode) | `FileNotFoundException` | `false` |
| Stat itself fails otherwise (permission denied on a component, I/O error) | `IOException` | `IOException` — still raises |
| Success | `FileStatus` | `true` + `status` |

One rule for "absent" — `FileNotFoundException` regardless of *which* component is missing — rather than .NET's `FileNotFound`/`DirectoryNotFound` split: the caller's question is "is something there," one question, one type; the exception message still names the path. (`DirectoryNotFoundException` remains §7.5's type for directory-*operation* failures, e.g. 0017's pattern listing; nothing here disturbs it.) The Try forms convert exactly the absent row to `false` — mirroring `TryParse`'s "false exactly where Parse raises" discipline (§2.5), narrowed to the fault that is an *expected outcome* of the question; an `IOException` is not an answer to "is something there," so it propagates in both forms.

## 8. Considered and declined

1. **The name `FileInfo`** (and the live path-object class behind it) — declined per D1: .NET's `FileInfo` is a stateful path object with `Refresh()` and verb methods; using its name for a pure snapshot invites its expectations. `FileStatus` names the stat.
2. **A `Path: string` field on the snapshot** — declined per D2: the caller has the path; carrying it breaks metadata equality for the sync/skip workload.
3. **`CreatedAt` / `AccessedAt`** — declined for v1: atime is unreliable by mount policy (`noatime`/`relatime` are the norm), and birth time has no POSIX guarantee (Linux `statx`-only, filesystem-dependent) — both would be fields that are silently garbage on common configurations, against the defined-behavior stance. Revisit with evidence of workloads that need them *and* a per-platform honesty story (likely nullable fields via `statx` capability detection).
4. **`ReadLink` (read a symlink's target)** — deferred, not declined: genuinely useful, but needs its own postures (relative targets resolved against what? chains followed how far?) and is separable — nothing in this surface depends on it.
5. **Handle-based stat (`FileStream.GetStatus()`)** — deferred: the race-free fstat companion for open files; `FileStream` already exposes `Length` (§7.1), which covers the observed need; add the full form when a real mtime-of-open-handle workload arrives.
6. **Full POSIX mode bits / owner ids / `IsExecutable`** — declined per D5: not cross-platform-defensible as shared surface; a future platform-extensions section is the home if evidence arrives.
7. **Async duals (`GetStatusAsync`)** — declined per §5, citing 0018 §7's recorded rule for metadata operations.
8. **Try shape (b) (`TryGetStatus(path): FileStatus?`)** — considered, and raised as open question 6 rather than silently declined: stdlib §1's own criterion for shape (b) ("the payload is statically non-nullable, so a bool would carry no information") applies here verbatim, and shape (c) hands a result-ignoring caller a plausible-looking zero (`default(FileStatus)` reads as an empty regular file — `FileKind.File` is the zero constant, so §17.11's zero-constant error, a warning before v0.242, does not fire). Shape (c) is proposed anyway for the spelling parallel with the `TryParse` family and the `out var` idiom the §9 examples lean on; note `FileStatus` would be the first user-visible record payload in shape (c) (every current shape-(c) out is a primitive or value builtin).
8. **Per-fact statics (`File.GetSize`, `File.GetLastWriteTime`)** — declined per D1: N syscalls, mutually-inconsistent reads, and a wider surface for the same information.

## 9. Tour by example

```bezel
import System.IO;

// --- Cache invalidation (build tools; 0040's scan-phase early-out) ---
public method NeedsRebuild(sourcePath: string, outputPath: string): bool
  if not File.TryGetStatus(outputPath, out var built) then
    return true;                                  // no output yet — expected, not a fault
  end;
  var src := File.GetStatus(sourcePath);          // missing source IS a fault: let it raise
  return src.ModifiedAt.CompareTo(built.ModifiedAt) > 0;   // DateTime tick-order comparison (§2.5)
end NeedsRebuild;

// --- Log rotation (the 0021 sink side) ---
if File.GetStatus(logPath).Size > maxBytes then
  File.Move(logPath, rotatedPath);
end;

// --- Existence-with-kind: one call, no Exists/Exists race ---
if File.TryGetStatus(path, out var entry) then
  case entry.Kind of
    FileKind.Directory => Descend(path),
    FileKind.File      => Index(path, entry.Size),
    _                  => Skip(path)
  end;
end;

// --- Directory walk with sizes (composes with 0017's pattern listing) ---
using var files := Directory.GetFiles(root);
var total: long := 0;
for var f in files do
  if File.TryGetStatus(f, out var fst) and fst.Kind = FileKind.File then
    total := total + fst.Size;
  end;
end;

// --- Symlink safety in a deploy tool: the no-follow form (D4) ---
if File.TryGetLinkStatus(candidate, out var lst) and lst.Kind = FileKind.Symlink then
  Skip(candidate);                                // never follow links out of the release tree
end;

// --- Cheap validator for static serving (0002's deferred lane) ---
var st2 := File.GetStatus(fullPath);              // FileNotFoundException → the 404 path
var etag := $"\"{st2.Size}-{st2.ModifiedAt.Ticks}\"";
```

## 10. Open questions for the owner

1. **Naming** — `FileStatus`/`GetStatus`/`GetLinkStatus` proposed (D1/D4); `FileMetadata`/`GetMetadata` is the runner-up. Confirm (the .NET name `FileInfo` is argued against in §8#1).
2. **`Size` pinned to 0 for `Directory`/`Other`** — proposed for portability (platform `st_size` for directories is filesystem-dependent noise). Alternative: surface the raw platform value and document it as unspecified — which would be the library's first deliberately-unspecified field. Confirm the pin?
3. **`IsReadOnly` mapping** (POSIX owner-write bit / Windows ReadOnly attribute, the .NET-compatible reading) — confirm, or drop the field entirely and ship kind/size/mtime only?
4. **mtime type** — `DateTime` with `Kind = Utc` now, `DateTimeOffset` accessor as a compatible widening after 0012 lands (D3). Confirm the sequencing posture?
5. **One-type "absent" fault** — `FileNotFoundException` for every missing-component case (§7), vs. .NET's `FileNotFound`/`DirectoryNotFound` split. Confirm the single-type rule?
6. **Try shape** — shape (c) (`out status: FileStatus`) is proposed; shape (b) (`TryGetStatus(path): FileStatus?`) fits stdlib §1's shape-(b) criterion equally well and avoids the plausible-zero hazard §8#8 records. Confirm shape (c), or direct shape (b)?

## 11. Relationship to existing proposals and issues

- **Proposal 0017** — the adjacent io-fs proposal; §6 states the no-overlap seam and composition.
- **Proposal 0018** — the async-family convention; its §7 rule ("no async dual for metadata operations") is applied and cited in §5.
- **Proposal 0012** — `DateTimeOffset` (accepted, unlanded): D3 cites its status and defines the lossless bridge; no dependency taken.
- **Proposal 0016** — `Instant`: D3 records why the monotonic type is the wrong mtime type by its own charter; the truncation-direction divergence is stated.
- **Proposal 0010** — the serialization dependency gating a future `FileStatus` conformance (§7).
- **Proposals 0002 / 0021 / 0040** — downstream consumers named in §2 (static serving, log rotation, build scanning).
- **[Stdlib gap review 2026-07](stdlib-gap-review-2026-07.md)** — not among the 31 items; the io-fs domain's #1/#22/#25 are each shown not to cover metadata (§0). This proposal extends the map.
- **[DECLINED.md](DECLINED.md)** — checked: no conflict (the gap review's eight declines and D-0001…D-0004 touch none of this ground).
