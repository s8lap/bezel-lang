# Proposal 0013 — Environment surface: `CurrentDirectory`, `ProcessId`, `OperatingSystem`

| | |
|---|---|
| **Status** | accepted — accepted (2026-07-07, delegated ruling — owner review pending) |
| **Target module** | `System` (auto-imported): additions to `Environment` (§2.4) + new `OperatingSystem` static class |
| **Written against** | spec v0.177 / stdlib v0.165 |
| **Tracking issue** | [#1441](https://github.com/s8lap/bezel-lang/issues/1441) |
| **Currency note** | Syntax refreshed to v0.228 spellings (2026-07-28 sweep); re-verified at v0.306 (2026-09-17 sweep) with no tier-1 respell applied. **Pinned-spelling record (v0.231, the changelog's own instruction for this file):** stdlib §2.4 renamed `GetEnvironmentVariable`→`Get`, `SetEnvironmentVariable`→`Set`, `GetEnvironmentVariables`→`Variables`, and the v0.231 migration entry directs that proposals 0011, 0013 and 0045 "keep their pinned long-name spellings and take a currency note at their next revalidation, not a retro-edit" — this is that note; read every `Get*EnvironmentVariable*` below as the v0.177 spelling of the current member. §2.4 has also since gained `GetInt`/`GetBool` and `Exit`, so §1's "already exists" enumeration is a pinned, not a current, list. `CurrentDirectory`, `ProcessId` and `OperatingSystem` are still absent from the landed stdlib, so the gap this proposal fills is open. Written-against pin unchanged. |
| **Gap-review item** | #12 (P0+2×P1) in [stdlib-gap-review-2026-07.md](stdlib-gap-review-2026-07.md) |

---

## 1. What already exists and what is missing

**Already in stdlib §2.4.** `Environment` in `System` (auto-imported) already exposes:

- `GetEnvironmentVariable(name: string): string?` — returns `null` when the variable is absent; the null return is the Try-convention shape (b) (§1, v0.171), so no separate `TryGetEnvironmentVariable` is needed.
- `SetEnvironmentVariable(name: string, value: string?)` — sets or clears; passing `null` removes the variable.
- `GetEnvironmentVariables(): owned Dictionary<string, string>` — snapshot of the full environment.
- `MachineName`, `UserName`, `OSVersion`, `ProcessorCount`, `CommandLineArgs`, `NewLine`.

**Missing, blocking real programs:**

1. `Environment.CurrentDirectory` — every program that reads relative paths or reports its working directory needs this. Get-only: the setter is declined (§2 below).
2. `Environment.ProcessId` — needed for PID files, log correlation, spawning diagnostics. One-liner addition.
3. `OperatingSystem.IsLinux() / IsWindows() / IsMacOS()` — runtime platform checks for conditional logic (opening the right config path, choosing a socket type, etc.).

`OperatingSystem` does not exist in the stdlib today. The gap review (item 6) records that **compile-time platform branching (`#ifdef`-style)** is declined as a language change; runtime methods are the approved mechanism.

---

## 2. Declined alternatives

**`CurrentDirectory` setter (gap review, declined item 4).**
`CurrentDirectory` is process-global state visible to every domain simultaneously. Setting it from one domain affects path resolution in every other domain without any synchronization — exactly the §22.12 / §30.14 hazard that the statics-and-domains rules exist to prevent. The setter would need to be an internally-synchronized type (§22.12 rule 2) to be safe, but a `string`-valued property cannot satisfy that. The getter is safe because reads are race-free on an immutable-for-the-reader string.

Child processes (proposal wave D2 gap #5) take an explicit `workingDirectory: string` parameter — that is the right place to control a child's working directory.

**`SetEnvironmentVariable` ban.** The setter already exists in §2.4 and is not re-litigated here. It is a process-global write subject to §22.12 rule 2's restriction to synchronized types — strictly, `string?` is not synchronized, so this is an existing inconsistency the statics-and-domains rules create. Recorded: **open question** for the owner (§5 below), not newly introduced by this proposal.

**`OperatingSystem.IsLinux(s: string)` / version checks.** Declined. Version-gated platform checks couple application code to OS version strings in a fragile way. A boolean "am I on Linux?" is the safe minimum.

---

## 3. `OperatingSystem` home and sendability

`OperatingSystem` is a process-level static class, in `System` alongside `Environment` and `Console` (§2.1). Auto-imported. All three methods call no Bezel code and return an immutable value — they are as safe from any domain as `Environment.ProcessorCount`.

---

## 4. Proposed stdlib additions (ready-to-land text)

### Addition to `Environment` (§2.4)

The following two properties are added to `Environment`:

```bezel
  // The process's current working directory as an absolute path string.
  // Reflects the OS-level current directory at the time of each read.
  // Getter only — the setter is declined (process-global mutable state,
  // §22.12 / §30.14 hazard; record in the gap review's declined item 4).
  public static readonly property CurrentDirectory: string;

  // The OS-assigned process identifier (PID) for the current process.
  // Stable for the lifetime of the process.
  public static readonly property ProcessId: int;
```

**`CurrentDirectory`** reflects the OS-level cwd at the time of the read (matches `getcwd` / `GetCurrentDirectory`). On platforms where the cwd can be changed by native code via FFI, subsequent reads may return a different path; this is expected.

**`ProcessId`** is stable after process startup. On POSIX this is `getpid()`. On Windows it is the Windows Process ID (not a handle).

### Proposed new §2.5 — `OperatingSystem`

*(Existing §§2.5–2.19 shift one number; docs editor adjusts at integration time.)*

```bezel
public static class OperatingSystem
  // Runtime platform queries. Each returns true when the running process is
  // executing on the named OS; false otherwise. Evaluated at run time —
  // there is no compile-time platform branching (gap-review declined item 6:
  // that is a language change requiring the spec workflow, not a stdlib addition).
  public static method IsLinux(): bool;
  public static method IsWindows(): bool;
  public static method IsMacOS(): bool;
end OperatingSystem;
```

`OperatingSystem` is in `System` and auto-imported. No import statement is required.

**Usage pattern:**

```bezel
var configDir: string;
if OperatingSystem.IsLinux() then
  configDir := Environment.GetEnvironmentVariable("HOME")! + "/.config/myapp";
elif OperatingSystem.IsWindows() then
  configDir := Environment.GetEnvironmentVariable("APPDATA")! + "\\myapp";
else
  configDir := Environment.CurrentDirectory + "/config";
end;

Console.WriteLine($"PID {Environment.ProcessId}: config dir = {configDir}");
```

(The `!` after `GetEnvironmentVariable` is required until §22.6 flow-narrowing; the existing Try-convention §1 note applies.)

---

## 5. Open questions for the owner

1. **`SetEnvironmentVariable` and §22.12.** `SetEnvironmentVariable(name, value)` already exists in §2.4 and is a process-global write of a `string?` value. Under §22.12 rule 2, a mutable static can only hold an internally-synchronized type; a method on a static class that *mutates* process-global state via a syscall is a grey area not covered by the rule as written. Should this be called out as an existing anomaly to resolve (e.g. restrict to the main domain only, or declare it "synchronized by the OS"), or left as-is?

2. **`IsMacOS()` naming.** The gap review uses `IsMacOS`. Is the capital `OS` acceptable (matching `OSVersion` in §2.4 and the .NET spelling), or should it be `IsMacos()` following the non-.NET acronym rule (v0.165 rationale, #1359)?

## Rulings recorded at acceptance (2026-07-07, delegated ruling — owner review pending)

The two §5 questions are resolved as follows; the additions (`CurrentDirectory`/`ProcessId` getters, `OperatingSystem`) land as §2.4/§2.5 amendments through the normal docs workflow.

- **Q2 — spelling is `IsMacOS()`.** Capital `OS` matches `OSVersion` in §2.4 (an existing acronym-cased member) and the gap-review spelling; the non-.NET acronym rule (#1359) does not override an already-established local convention for `OS`.
- **Q1 — `SetEnvironmentVariable` × §22.12: do not block this proposal; fold into the statics-×-domains ruling context (#1363).** The observation is genuine — a static method that mutates process-global `string?` state via a syscall is a grey area the §22.12 rule-2 wording (mutable-static admission restricted to internally-synchronized *types*) does not cleanly cover — but it concerns a *pre-existing* §2.4 member, not anything this proposal introduces. It is folded into the #1363 statics-×-domain-isolation work rather than opened as a new issue, with the likely resolution being to document `SetEnvironmentVariable` as **OS-synchronized** with an explanatory §22.12 note. This proposal's getters (`CurrentDirectory`, `ProcessId`) are race-free reads and are unaffected.

Coordination note carried forward: the proposed `new §2.5` renumbering collides with 0012's insertions into the §2.5 cluster — the docs editor resolves the numbering at integration, as both proposals already state.
