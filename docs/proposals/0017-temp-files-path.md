# Proposal 0017 — `TempFile` RAII + `Path`/`Directory` utility completions

| | |
|---|---|
| **Status** | under-review — draft |
| **Target module** | `System.IO` (stdlib §7 — new §7.3a `TempFile`; §7.4 `Path` and `Directory` extensions) |
| **Written against** | spec/stdlib **v0.177** |
| **Currency note** | Refreshed 2026-09-17 to v0.306: §1's transcribed `Directory` surface conformed to the v0.290 overload-pair rule (stdlib §1; the `Delete(path)` / `Delete(path, recursive)` pair is one `:=`-defaulted signature, stdlib §7.4). `TempFile.Take(owned TempFile)` re-verified as a declaration-position signature sketch (unchanged). No tier-2 flags. Written-against pin unchanged. |
| **Tracking issue** | [#1446](https://github.com/s8lap/bezel-lang/issues/1446) |
| **Gap-review item** | #25 (2026-07 ranking, wave D2); also closes the discharge-by-move ruling the review requested |

---

## 1. What exists today (the substrate this builds on)

All items below are in the current stdlib; nothing is invented here.

**`Path`** (stdlib [§7.4](../Bezel_Standard_Library.md#sec-textreader-textwriter-directory-path)):

```
Combine(a: string, b: string): string
GetFileName(path: string): string
GetExtension(path: string): string            // "" when none
GetFileNameWithoutExtension(path: string): string
GetDirectoryName(path: string): string
GetFullPath(path: string): string
```

**`Directory`** (stdlib §7.4):

```
Exists(path: string): bool
CreateDirectory(path: string)
Delete(path: string, recursive: bool := false)   // IOException if non-empty and not recursive
GetFiles(path: string): owned Array<string>
GetDirectories(path: string): owned Array<string>
```

**`File`** (stdlib [§7.3](../Bezel_Standard_Library.md#sec-file)) — `Exists`, `Delete`, `Move`, `Copy`, and the read/write family.

**IO exceptions** (stdlib §7.5) — `IOException`, `FileNotFoundException`, `DirectoryNotFoundException`. Raised on failure.

**What is missing:** a temporary file abstraction with automatic cleanup; `Path.ChangeExtension`, `Path.GetTempPath`, variadic `Path.Combine`; `Directory.GetFiles` with a pattern (glob), `Directory.CreateTempSubdirectory`, `Directory.Move`.

---

## 2. `TempFile` — ownership, disposal, and the "discharge by move" ruling

### 2.1 Shape

```bezel
/// An owned temporary file that deletes itself when disposed.
/// Obtain via TempFile.Create(); hold with `using var`.
/// To keep the file past scope end, call Persist() before disposal.
public class TempFile
  // Creates a new empty temporary file in Path.GetTempPath() (PROPOSED).
  // The file name is chosen by the OS (guaranteed unique).
  public static method Create(): owned TempFile;

  // Creates a new empty temporary file in the specified directory.
  // Raises IOException if the directory does not exist.
  public static method Create(directory: string): owned TempFile;

  // The absolute path of the temporary file.
  // Valid for the lifetime of this TempFile object.
  public readonly property Path: string;

  // Disarms automatic deletion and returns the path.
  // After Persist() returns, the eventual Dispose() of this TempFile
  // is a no-op with respect to the file on disk.
  // Calling Persist() twice is idempotent.
  public method Persist(): string;

  // Deletes the file NOW, at a named point, and surfaces any IOException to
  // the caller. Idempotent (§28.2): a second Delete(), or the destructor
  // firing after Delete(), is a no-op. Use this when a deletion failure must
  // be observed; the destructor's best-effort delete never reports one.
  public method Delete();

  destructor;   // best-effort delete unless Persist()/Delete() already ran;
                // swallows and LOGS all deletion failures — a destructor must
                // not throw (§28.4)
end TempFile;
```

### 2.2 Disposal posture: swallow, not fault, on missing file

The destructor is **best-effort and never throws.** When it fires it attempts the deletion (unless `Persist()` or an explicit `Delete()` already ran), and it **swallows and logs every deletion failure** — the already-gone file (`FileNotFoundException`), a permission error, a path-too-long — rather than propagating any of them. Rationale: spec [§28.4](../Bezel_Language_Spec.md#sec-the-destructor) states that disposers are **expected not to throw**; a destructor that raised `IOException` would violate that no-throw discipline, and would make `using var` fragile in pipelines where the file might have been consumed (moved into place by an external tool, deleted by another process). This is the same best-effort posture 0006's `StreamWriter` takes on its sync-on-dispose path. §28.2 idempotency still holds — a deletion already performed (by `Delete()`, `Persist()`+external move, or another process) makes the destructor's attempt a no-op. A caller who **needs** a deletion failure surfaced calls the explicit `Delete()` method (§2.1), which raises `IOException` at a named point.

### 2.3 The "discharge by move" ruling — `Persist()`

The gap review explicitly called for a "discharge-by-move ruling": a way to escape the auto-deletion without a raw `Dispose` call, analogous to how `move` transfers ownership in spec [§28.8](../Bezel_Language_Spec.md#sec-ownership-transfer). In Bezel, class instances do not support `move` in the inline-array sense; the ownership model is single-owner via the `using var`/`owned` annotations. The solution is a **consumer method** — `Persist()` — that disarms the destructor's deletion behavior in place, then returns the path.

```bezel
// Usage: keep the temp file after scope end
var finalPath: string;             // non-nullable local, assigned below (definite assignment, §7.9)
do
  using var tmp := TempFile.Create();
  WriteReport(tmp.Path);
  finalPath := tmp.Persist();    // disarms deletion, tmp.Dispose() is now a no-op
end;   // tmp is disposed here — file is NOT deleted
Console.WriteLine($"Report written to {finalPath}");
```

`Persist()` does NOT dispose or null out the `TempFile` — the `using var` binding remains valid and `Path` continues to return the path. Only the deletion behavior is disarmed.

**Alternative considered:** a static "discharge-to-ownership" method `TempFile.Take(owned TempFile): string` that consumes the object. This maps more directly onto `move` semantics but requires transferring the owned binding, which is syntactically heavier (callers need to `move` the local). The instance method `Persist()` is simpler and achieves the same result with no additional language machinery.

### 2.4 Usage examples

```bezel
// --- Pattern 1: temp file for inter-process handoff ---
using var tmp := TempFile.Create();
File.WriteAllBytes(tmp.Path, GenerateData());
ExternalTool(tmp.Path);
// tmp.Dispose() fires at scope end, deletes the file

// --- Pattern 2: keep the file (discharge-by-move ruling) ---
var kept: string;                  // assigned inside the scope below (§7.9)
do
  using var tmp := TempFile.Create();
  File.WriteAllText(tmp.Path, BuildConfig());
  kept := tmp.Persist();           // disarms deletion, returns path
end;
// file at `kept` persists on disk

// --- Pattern 3: create in a specific directory ---
using var tmp := TempFile.Create(workDir);
```

---

## 3. `Path` utility completions

Read the existing §7.4 surface first (listed in §1 above). The following members are **absent** and are proposed:

```bezel
// Extensions to Path (System.IO, auto-imported with System.IO import):
public static class Path
  // ... (existing members unchanged)

  // Returns path with its extension replaced by `extension`.
  // If `extension` does not start with '.', one is prepended.
  // If `extension` is "" or null, removes the extension.
  // Path.ChangeExtension("/a/b.txt", ".bak") → "/a/b.bak"
  // Path.ChangeExtension("/a/b.txt", "")    → "/a/b"
  public static method ChangeExtension(path: string, extension: string): string;   // PROPOSED

  // Returns the platform temp directory (e.g. "/tmp" or "C:\\Temp\\").
  // Always ends with the platform directory separator.
  // Equivalent to: the TMPDIR/TMP/TEMP environment variable chain, or /tmp.
  public static method GetTempPath(): string;   // PROPOSED

  // Three-component join (otherwise identical semantics to Combine(a, b)).
  public static method Combine(a: string, b: string, c: string): string;   // PROPOSED

  // Array-form join: combines all segments in order.
  // Combine(new Array<string>(0)) → ""; Combine(["a"]) → "a".
  public static method Combine(parts: Array<string>): string;   // PROPOSED
end Path;
```

**What is NOT proposed:** `GetRandomFileName()` (requires a random number source — propose separately if needed), `IsPathRooted()` (trivially expressible as `path.Length > 0 and (path[0] = '/' or ...)`).

**`GetExtension`, `GetFileNameWithoutExtension` already exist** (verified in §7.4). Not duplicated here.

---

## 4. `Directory` utility completions

The following members are absent from §7.4 and are proposed:

```bezel
// Extensions to Directory (System.IO):
public static class Directory
  // ... (existing members unchanged)

  // Enumerates files matching a glob pattern.
  // Pattern uses '*' (any chars in one path segment) and '?' (one char).
  // No recursive glob (**) in v1 — pass `recursive: true` to Directory.Delete
  // for the recursive deletion analogue; a recursive-glob overload is deferred.
  // Raises DirectoryNotFoundException if `path` does not exist.
  public static method GetFiles(path: string, pattern: string): owned Array<string>;   // PROPOSED

  // Like GetFiles but returns subdirectory paths.
  public static method GetDirectories(path: string, pattern: string): owned Array<string>;   // PROPOSED

  // Creates a uniquely-named subdirectory under Path.GetTempPath()
  // (or under `baseDirectory` if supplied). Returns the absolute path.
  // The caller owns the resulting directory and must delete it when done.
  // Analogous to TempFile.Create() but for directories — no RAII wrapper
  // (directories require recursive deletion; callers use Directory.Delete(path, true)).
  public static method CreateTempSubdirectory(prefix: string := ""): string;                  // PROPOSED
  public static method CreateTempSubdirectory(baseDirectory: string, prefix: string := ""): string;   // PROPOSED

  // Moves a directory (and its contents) from source to destination.
  // Raises IOException if destination already exists.
  public static method Move(source: string, destination: string);   // PROPOSED
end Directory;
```

**Why no `TempDirectory` RAII?** A temporary directory requires recursive deletion at cleanup (`Directory.Delete(path, true)`). Making this safe as a `using var` type needs either a destructor that catches every exception during recursive deletion, or an explicit success/fail deletion mode. This is a meaningful design choice. For v1, `CreateTempSubdirectory` returns the path and leaves deletion to the caller; a `TempDirectory` RAII wrapper can follow once the usage pattern is established.

---

## 5. Placement of `TempFile` in the stdlib

`TempFile` is proposed as a new subsection **§7.3a** immediately after `File` (§7.3), before `TextReader`/`TextWriter` (§7.4). It lives in `System.IO` and is available when that module is imported. It is not auto-imported (a filesystem-side-effect type should require an explicit import).

`Path.GetTempPath()` returns the platform temp directory — this is where `TempFile.Create()` places its files by default, and it is the idiomatic entry point for "give me somewhere temporary to write."

---

## 6. Design decisions and alternatives

### Q1 — `TempFile` as `class`, not `record`

Records cannot have a `destructor`, `owned` fields, or implement `IDisposable` (spec [§9.7](../Bezel_Language_Spec.md#sec-records-and-disposal)). The deletion behavior requires a destructor. `TempFile` is therefore a class — an owned, disposable resource, exactly as `FileStream` is.

### Q2 — Swallow vs fault on a deletion failure in the destructor

Chosen: the destructor is **best-effort and never throws** — it swallows and logs *every* deletion failure (`FileNotFoundException`, permission errors, path-too-long). Rationale in §2.2 above. The earlier draft's "swallow FileNotFound, propagate other `IOException`" is rejected: propagating *any* exception from a destructor conflicts with §28.4 ("disposers are expected not to throw"), and it would make `TempFile` fragile in "create, write, hand path to tool, scope exit" patterns where the tool might consume the file (e.g., an installer that moves the temp file into place). Using `Persist()` is the correct signal for "I intend to keep this"; swallowing the deletion error is the fallback for "someone else cleaned it up" — a common, benign outcome. Callers who must observe a deletion failure at a named point call the explicit `Delete()` method (§2.1), which *does* raise `IOException`.

### Q3 — `ChangeExtension` with an empty `extension`

Returns the path with no extension (trailing dot removed if present). `ChangeExtension("a.tar.gz", ".gz")` → `"a.tar.gz"` (replaces only the last extension — matching .NET's behavior, which is the intuitive reading of "extension" as the portion after the last dot).

### Q4 — Recursive glob (`**`) deferred

Pattern-based `GetFiles` with `**` requires careful semantics around symlink following, depth limits, and cross-device traversal. This is a meaningful design space; v1 ships the single-level `*`/`?` pattern and defers `**` to a follow-on proposal.

---

## 7. Open questions for the owner

1. **`TempFile.Create()` on failure**: if the OS fails to create the temp file (disk full, permissions), `Create()` raises `IOException`. The object is not returned. This is straightforward — is there a concern about the error model here?
2. **`CreateTempSubdirectory` name**: `.NET 6` uses `Directory.CreateTempSubdirectory`; Go uses `os.MkdirTemp`. The .NET name is proposed for consistency; object if preferred otherwise.

## Delegated rulings 2026-08-11 (stdlib-wave readiness; owner review in repo)

Resolving the two §7 questions under the owner's 2026-08-11 delegation ("work autonomously... don't ask me for confirmations or rulings"; review-in-repo) (delegated ruling, 0012-precedent style). Status stays draft.

1. **Q1 — `Create()` raising `IOException` on OS failure: CONFIRMED, no error-model concern.** This is exactly `FileStream`'s throwing-constructor shape (stdlib §7.2), and under the v0.229 failure-signal rule an environmental fault (disk full, permissions) is a raising band, not a branch tier — a temp file that cannot be created is not an expected-absence the caller probes for. A `TryCreate` dual is not added until a probing consumer appears (the §1 Try-convention then applies); declined now as speculative surface.
2. **Q2 — `Directory.CreateTempSubdirectory`: CONFIRMED.** Consistency with the .NET-shaped `Directory`/`Path` surface (§7.3) outweighs brevity; Go's `MkdirTemp` has no local naming precedent to anchor to ("Mkdir" appears nowhere in the stdlib). Declined: `CreateTempDirectory` (reads as creating *the* temp directory, which `Path.GetTempPath` already locates — the .NET name's "Subdirectory" states the actual contract: a fresh directory *under* the temp root).

With these, this proposal carries no open design question; the remaining path is the ordinary acceptance/graduation workflow.

---

## 8. Relationship to existing issues and proposals

- Gap-review item #25; wave D2 per the [stdlib gap design plan](stdlib-gap-design-plan-2026-07.md).
- The gap review note: "discharge-by-move ruling" — settled by `Persist()` in §2.3 above.
- `Path.GetTempPath()` is the entry point for `TempFile.Create()` — the two additions are co-designed.
- `Directory.CreateTempSubdirectory` is the directory analogue; a `TempDirectory` RAII type is explicitly deferred.
