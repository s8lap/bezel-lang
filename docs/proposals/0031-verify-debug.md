# Proposal 0031 — `Verify`, `Unreachable`, and `Debug` additions

| | |
|---|---|
| **Status** | under-review — draft |
| **Tracking issue** | [#1456](https://github.com/s8lap/bezel-lang/issues/1456) |
| **Target module(s)** | `System.Diagnostics` (requires `import System.Diagnostics`); additions to `Debug` (existing §8.2) |
| **Written against** | Language spec **v0.177**; stdlib companion v0.165 |
| **Currency note** | Syntax refreshed to v0.228 spellings (2026-07-28 sweep); refreshed 2026-09-17 to v0.306: §5.1's transcribed `Debug` class conformed to the v0.290 overload-pair rule — the `Assert(condition)` / `Assert(condition, message)` pair is one `:=`-defaulted signature (stdlib §1, §8.2). §4's `throw`-bottom-type argument (§17.12), the A.6 `CatchArm` shape and `Exception.StackTrace` (§31.4) re-verified. ⚠ **Two tier-2 flags** recorded in `proposals-currency-2026-09-17.md` §3: (1) stdlib §8.2 now states that a failed `Debug.Assert` reports and **terminates the process** — *"`Assert` never throws"* — so §5.1's "`Debug.Assert(false, msg)` and `Debug.Fail(msg)` are equivalent semantically" and §5.2's "follows the same contract exactly" no longer hold of a `Fail` that raises a catchable `VerificationException`; (2) §5.1/§8's proposed `Fail(message)` / `Fail(message, detailMessage)` pair is the shape v0.290's rule collapses. Written-against pin unchanged. |
| **Origin** | Gap review [#1403](https://github.com/s8lap/bezel-lang/issues/1403) item #23 (2×P1+P2) |

---

## 1. Problem and motivation

Three closely related diagnostic tools are missing from the current surface:

1. **Always-on invariant checks** (`Verify.That`) — `Debug.Assert` (§8.2) is elided in release builds; there is no production-safe assertion that a program invariant holds.
2. **An unreachable-code marker** — for case arms, dead branches, and exhausted match paths that the compiler cannot statically prove unreachable. The only option today is `throw new InvalidOperationException("impossible")`, which is verbose and lacks a canonical name.
3. **`Debug.Fail`** — a named "unconditional debug-build failure" counterpart to `Debug.Assert`.

A fourth item in the gap review grouping is **`Debug.CaptureStackTrace`**. This proposal assesses it (§5) and defers it with the finding recorded.

---

## 2. Homes and naming

All new types live in `System.Diagnostics`, matching the existing `Debug` and `Stopwatch` module.

- A new **`Verify` static class** for always-on production invariant checks.
- A new **`VerificationException`** (in `System.Diagnostics`, requiring `import System.Diagnostics`) for the exception `Verify` raises.
- `Debug.Fail` is added to the **existing `Debug` class** (stdlib §8.2).

**Verify vs Assert.** The gap review notes that a concurrent proposal (0022, test framework) will introduce an `Assert` class for test assertions. To prevent confusion and coupling:
- `Verify` is for **production code** — always-on invariant checks that survive into release builds.
- `Assert` (proposal 0022, referenced by number only) is for **test code** — assertions that run inside the test framework and are meaningless outside it.
- The names are unambiguous: `Verify.That(cond, msg)` says "I am asserting a runtime invariant of this program"; `Assert.Equal(a, b)` says "I am asserting a test expectation."

---

## 3. `Verify.That` — always-on production invariant check

### 3.1 Semantics and return type

```bezel
public static class Verify
  // Raise VerificationException(message) if `condition` is false.
  // Always active — NOT elided in release builds, unlike Debug.Assert.
  // `condition` side-effects are always evaluated.
  public static method That(condition: bool, message: string := "");

  // (See §4 for Verify.Unreachable)
end Verify;
```

**Always-on** is the defining contrast with `Debug.Assert`. `Debug.Assert` may be elided in release builds (stdlib §8.2: "Calls to `Debug.*` may be elided in release builds, depending on the compiler's `--debug` flag"). `Verify.That` is never elided — it is the tool for invariants that must hold in production.

**Side effects.** Because `Verify.That` is never elided, `condition` is always evaluated. Unlike `Debug.Assert` (§8.2: "Side effects in `Debug.Assert` arguments are unspecified"), `Verify.That`'s argument evaluation is fully specified: the condition and message are always computed.

**Return type `()`** (unit). `Verify.That` is used as a statement:
```bezel
Verify.That(index >= 0 and index < Count, $"index {index} out of range");
Verify.That(not disposed, "method called after disposal");
```

### 3.2 `VerificationException`

```bezel
public class VerificationException: Exception
  public constructor(message: string);
end VerificationException;
```

Derives directly from `Exception` (spec §31.4). It is not a subtype of `InvalidOperationException` — a failed `Verify.That` is not "operation not valid in current state" (a recoverable condition); it is a program invariant violation (a logic bug, not a state error). Callers should generally not catch `VerificationException`; it is the "this can't happen and did" signal, analogous to Rust's panic.

`VerificationException` lives in `System.Diagnostics` and requires `import System.Diagnostics`. It is not promoted to the built-in exception hierarchy (spec §31.4) because its only producer is the `Verify` class, which is not a built-in operation. The test framework's `Assert` family (proposal 0022) will define its own `AssertionException` type; neither type is in the built-in hierarchy; both require their respective imports.

---

## 4. `Verify.Unreachable` — the expression-position idiom

### 4.1 The return-type problem: Bezel has no `never` type

In languages with a bottom/never return type (Rust's `!`, Swift's `Never`, TypeScript's `never`), an `unreachable()` function can be declared to return `never`, making the call expression bottom-typed at the call site. The compiler then treats any code after the call as unreachable.

**Bezel has no `never` return type for methods.** A search of spec v0.177 and the design rationale finds no `never`/`noreturn`/bottom-return annotation for method declarations. The bottom type exists only for **`throw` expressions** (spec §17.12): "Both forms have the bottom type — assignable to any expected type — because control transfers to the matching handler before any value is produced." This bottom type is attached to the `throw` keyword, not to the return type of any callable.

The absence is consistent with Bezel's conservative type-system posture: adding a `never` return type annotation would require new spec machinery (a new pseudo-type, new rules for how the compiler verifies a method never returns, new handling in the type-inference lattice). That is a language-level change, not a stdlib addition.

### 4.2 The factory pattern for expression position

Since `Verify.Unreachable` cannot itself be bottom-typed, the next best option is to make it a **factory** that returns a `VerificationException` — without throwing it. The caller's `throw` keyword then provides the bottom type:

```bezel
_ => throw Verify.Unreachable("should be unreachable: foo was neither A nor B")
```

Here, `throw Verify.Unreachable(...)` is a `throw expr` expression (spec §17.12), which IS bottom-typed and resolves the arm's type constraint.

```bezel
public static class Verify
  // Returns a new VerificationException with the given message.
  // Does NOT throw — the caller's `throw` keyword does.
  // Intended use: `throw Verify.Unreachable(msg)` — the outer `throw` is
  // what makes the expression bottom-typed (spec §17.12). The `owned` return
  // satisfies the v0.201 factory-announcement rule (a fresh instance whose
  // ownership transfers to the caller); `throw` is an owned-consuming
  // position, so the intended idiom is unchanged.
  public static method Unreachable(message: string := "unreachable"): owned VerificationException;
end Verify;
```

### 4.3 The intended call sites

**Expression position (the primary use case):**
```bezel
// In a case arm where all logical cases are covered but the compiler cannot prove it:
var label: string := (case direction of
  Directions.North => "north",
  Directions.South => "south",
  Directions.East  => "east",
  Directions.West  => "west",
  _ => throw Verify.Unreachable($"unexpected direction: {direction}"));

// In an if-else where an else branch cannot actually be reached:
var kind := (if tokens.IsEmpty then
  TokenKind.End
elif tokens[0].IsKeyword then
  TokenKind.Keyword
else
  throw Verify.Unreachable("non-keyword token fell through all branches"));
```

**Statement position:**
```bezel
// Used as a statement when the unreachable point is in a void-returning context:
throw Verify.Unreachable("control should not reach here after early return");
```

The `throw` keyword is always required. Calling `Verify.Unreachable(msg)` without `throw` returns a `VerificationException` instance and discards it — a bug. The naming `Unreachable` (not `MakeUnreachableException`) is chosen because the call site `throw Verify.Unreachable(msg)` reads as a unit: "this is an unreachable throw."

### 4.4 Why not `Verify.That(false, msg)`

`Verify.That(false, msg)` works correctly as a statement for the same purpose, but:
1. It reads as a condition-check that always fails, not as a "this point should not be reached" declaration.
2. It returns `()`, so it cannot be used in an expression-position case arm.
3. `throw Verify.Unreachable(msg)` makes the intent clear and works in both statement and expression position.

### 4.5 Future `never` return type

If Bezel eventually adds a `never` return type annotation (a spec change that would require new spec machinery), `Verify.Unreachable` could be updated to throw internally and declare a `never` return. The calling pattern `throw Verify.Unreachable(msg)` would continue to compile (the double-throw is a no-op; the outer `throw` never executes). This forward-compatibility is a feature of the factory pattern.

---

## 5. `Debug.Fail` — unconditional debug-build failure

### 5.1 Relationship to existing `Debug.Assert`

The existing `Debug` class (stdlib §8.2):
```bezel
public static class Debug
  public static method Assert(condition: bool, message: string := "");
  public static method WriteLine(message: string);
end Debug;
```

`Debug.Assert(false, msg)` and `Debug.Fail(msg)` are equivalent semantically: both are unconditional failure signals in debug builds. `Debug.Fail` is the named canonical form:

```bezel
// Addition to the existing Debug class (§8.2):

  // Unconditional failure in debug builds; elided in release builds.
  // Equivalent to Debug.Assert(false, message).
  // Raises VerificationException in debug builds; is a no-op in release builds.
  public static method Fail(message: string);
  public static method Fail(message: string, detailMessage: string);
end Debug;
```

### 5.2 Build-config behavior: the honest definition

Gap-review decline #6 says: "Compile-time platform branching — a language change, not a stdlib addition; runtime `OperatingSystem.IsLinux()` queries cover the stdlib-scoped need."

This decline applies to compile-time `#ifdef`-style platform branching; it does NOT say that the compiler's `--debug` flag is unavailable. The existing `Debug.Assert` already has a specified debug/release behavior (stdlib §8.2). `Debug.Fail` follows the same contract exactly:

- **Debug build** (`--debug` flag): `Debug.Fail` raises `VerificationException`.
- **Release build** (no `--debug` flag): `Debug.Fail` is a no-op (elided by the compiler).

The behavior is controlled by the same compiler flag that governs `Debug.Assert`. This is NOT a new build-config mechanism; it is an extension of the existing Debug-elision contract.

**Exception type:** `Debug.Fail` raises `VerificationException` (this proposal's new exception type) rather than `InvalidOperationException`. The choice is intentional: in a debug build, a `Debug.Fail` hit is a programming invariant violation, not a state error. Using a specific `VerificationException` makes the failure origin identifiable in crash logs.

### 5.3 Side effects

Like `Debug.Assert`, side effects in `Debug.Fail` message arguments are unspecified in release builds (the arguments may not be evaluated when the call is elided). Document this explicitly, matching §8.2's existing note.

---

## 6. `Debug.CaptureStackTrace` — finding and deferral

The gap review groups `Debug.CaptureStackTrace` alongside `Verify` and `Debug.Fail`. This section records what the spec's exception machinery provides and why a standalone `CaptureStackTrace` is deferred.

### 6.1 What the spec already provides

Spec §31.4 defines `Exception.StackTrace: string` — a formatted stack-trace string, populated automatically when an exception is thrown and caught. Stdlib §6.8 (`Domain`): `DomainFaultedException.FaultStackTrace: string` carries the stack trace across a domain boundary as a plain string.

Stack traces in Bezel are therefore **exception-attached**: they are produced automatically at throw time, accessible through the caught exception's `StackTrace` property, and copyable as `string` values for logging. The idiom for capturing a stack trace for logging is to catch an exception, copy its `StackTrace`, and rethrow:

```bezel
try
  // ... something
catch
  var ex: SomeException => do
    logEntry.StackTrace := ex.StackTrace;  // copy before the arm ends
    throw;                                  // re-raise preserving the trace
  end
end;
```

Or, when the trace is needed outside a catch arm, deliberately create and throw a sentinel exception:
```bezel
// Not idiomatic but functional — captures a trace without an underlying failure:
var trace: string := "";
try
  throw new VerificationException("stack probe");
catch
  var ex: VerificationException => do trace := ex.StackTrace; end
end;
```

### 6.2 Why a standalone API is absent

There is no `Environment.StackTrace`, no `new StackTrace()`, and no `Debug.CaptureStackTrace()` API currently in the spec or stdlib. The spec machinery attaches traces to exceptions at throw time; there is no mechanism to capture a trace at an arbitrary point without throwing.

Adding such an API would require either:
- A new runtime primitive that walks the call stack and returns a string — a non-trivial runtime addition not justified by a P2 gap.
- An intrinsic `throw`+`catch` pair that the compiler can optimize — possible but spec-level work.

**Finding recorded:** The exception machinery (spec §31.4) provides stack traces through caught exceptions, which covers the primary diagnostic use case (logging failure causes). A standalone "capture at any point" API is not grounded in the current spec machinery and is deferred until a concrete consumer articulates the requirement.

---

## 7. Declined alternatives (register)

1. **Placing `Verify` and `VerificationException` in `System` (auto-imported)** — declined. Invariant checks are a diagnostic tool, not a fundamental language primitive. Auto-importing adds `Verify`, `VerificationException` to every compilation unit's namespace. The `import System.Diagnostics` friction is small and signals that the programmer is reaching for a diagnostic surface.
2. **`VerificationException` in the built-in exception hierarchy (§31.4)** — declined. §31.4's principle: "an exception is built-in iff its only producer is a built-in operation." `VerificationException` is produced by `Verify`, a library class, not a built-in operation. Promoting it would break the principle.
3. **`Verify.Unreachable` throws internally with `()` return type** — declined. A `()` return type means `Verify.Unreachable(msg)` cannot be used in expression-position case arms (spec §17.12's bottom type is attached to `throw`, not to a `()` return). The factory pattern (returns `VerificationException`, caller writes `throw Verify.Unreachable(msg)`) preserves the bottom-type property through the existing `throw` mechanism without needing a new `never` type.
4. **Naming `Verify.Unreachable` as `Verify.CannotReachHere` or `Verify.Impossible`** — declined. `Unreachable` is the established name across the ecosystem (Rust `unreachable!()`, C# `Debug.Assert(false, "Unreachable")`, Java `assert false: "unreachable"`). `throw Verify.Unreachable(msg)` reads naturally as an idiom.
5. **`Debug.Fail` raising `Exception` instead of `VerificationException`** — declined. `VerificationException` is the appropriate typed signal for a deliberate invariant-failure; a bare `Exception` would be caught by overly broad catch arms.
6. **`CaptureStackTrace` as a standalone API in v1** — deferred per §6.2. No grounding in the current spec exception machinery; the P2 priority does not justify a new runtime primitive.
7. **`Verify.That` using `out` parameter for the message** — declined; the condition/message ordering matches `Debug.Assert`'s existing two-argument form and all common assertion library conventions.

---

## Delegated rulings 2026-08-11 (stdlib-wave readiness; owner review in repo)

Resolving the two standing design flags from the 2026-07-28 currency sweep (flags 4–5) and folding the v0.243 premise, under the owner's 2026-08-11 delegation ("work autonomously... don't ask me for confirmations or rulings"; review-in-repo) (delegated ruling, 0012-precedent style). Status stays draft.

1. **No exception-factory exemption from the v0.201 factory-announcement rule; `Verify.Unreachable` is declared `owned VerificationException`.** The sweep asked whether factories whose result is conventionally immediately thrown are exempt from the announcement rule. Ruled **no exemption — announce**: the v0.201 rule's whole value is totality (every static class-returning signature announces its ownership contract), and an "immediately-thrown by convention" class is not signature-decidable — admitting it would re-open the instance↔static refactor hazard the rule closed, for one method's cosmetics. The announced form costs the intended idiom nothing: `throw` is an **announced owned-consume** (spec §31.1 — the same no-marker family as the consuming discard, §17.13), so `throw Verify.Unreachable(msg)` is unchanged, and the exception's disposal after the matching catch arm is the ordinary §31.1 lifecycle. Strictly better, in fact: a call that is *not* thrown now leaves an owned local the standard §28.3 discharge machinery flags — the "constructed and discarded a `VerificationException`" bug §4.3 warns about becomes compiler-visible, where an exemption would leave it silent. The §4.2 sketch already carried the `owned`; §8's ready-to-land text is conformed in place (it lacked the marker).
2. **v0.243 premise fold (proposal 0080): strengthens, does not disturb.** Every throw operand now derives from `Exception`; `VerificationException: Exception` satisfies it, the §31.4 built-in-iff-built-in-producer placement argument is untouched, and §6.1's probe idiom stays legal. §4.1's finding (no `never` return type) is likewise unmoved — v0.243 changed the operand's type bound, not the type system's bottom; the factory-plus-`throw` idiom remains the sanctioned expression-position spelling.
3. **The sweep's flag 4 (catch-arm spelling) is verified current.** §6.1's arms are the A.6 `CatchArm` shape (`var ex: T => do … end`); no residual pre-v0.207-era catch spelling remains in this document.
4. **`Debug.CaptureStackTrace` deferral stands as recorded** (§6.2): evidence bar unchanged — a concrete consumer articulating the capture-at-any-point requirement, at which point it is runtime-primitive design, not stdlib surface.

With these, this proposal carries no open design question; the remaining path is the ordinary acceptance/graduation workflow.

## 8. Proposed stdlib additions (ready-to-land text)

The following is written to be inserted into `Bezel_Standard_Library.md`. Section numbers assigned at integration time.

**Addition to §8.2 `Debug` (existing section — add `Fail` overloads):**

```
  // Unconditional failure in debug builds.
  // Elided in release builds (same conditions as Debug.Assert — §8.2).
  // When active, raises VerificationException (System.Diagnostics).
  // Side effects in message arguments are unspecified in release builds.
  public static method Fail(message: string);
  public static method Fail(message: string, detailMessage: string);
```

**New section (insert into §8 System.Diagnostics, after §8.2 Debug):**

```
### 8.3  `VerificationException`

The exception raised by `Verify.That` and, in debug builds, by `Debug.Fail`.
Signals a violated program invariant — a logic bug, not a recoverable error.
Programs should generally not catch `VerificationException`; it indicates that
an assumption the code depended on has been falsified.

    public class VerificationException: Exception
      public constructor(message: string);
    end VerificationException;

Requires `import System.Diagnostics`.

---

### 8.4  `Verify`

A static class for production invariant checks.  Unlike `Debug.Assert` (§8.2),
`Verify` methods are **never elided** — they execute identically in debug and
release builds, and `condition` is always evaluated.

    public static class Verify
      // Raise VerificationException(message) if `condition` is false.
      // `message` defaults to the empty string when omitted.
      public static method That(condition: bool, message: string := "");

      // Return a new VerificationException with the given message.
      // Does NOT throw.  Intended use: `throw Verify.Unreachable(msg)`.
      // The outer `throw` expression (spec §17.12) is bottom-typed, so
      // `throw Verify.Unreachable(msg)` may appear in any expression position
      // (case arm, if-else branch, etc.) where an arbitrary type is expected.
      //
      // Bezel has no `never` return type for method declarations (spec v0.177);
      // the factory pattern is the v1 idiom.  If a future spec revision adds
      // a `never`/noreturn annotation, this method's signature will be revisited;
      // the calling pattern `throw Verify.Unreachable(msg)` will remain valid.
      //
      // `owned`: the v0.201 factory-announcement rule (ruling 2026-08-11, above).
      // `throw` is an announced owned-consume (spec §31.1), so the idiom is
      // unchanged; an un-thrown call leaves an owned local the §28.3 discharge
      // machinery flags.
      public static method Unreachable(message: string := "unreachable"): owned VerificationException;
    end Verify;

Requires `import System.Diagnostics`.

Usage:

    // Invariant check (statement form):
    Verify.That(buffer.Capacity > 0, "buffer must be non-empty before write");

    // Unreachable branch (expression form — `throw` provides the bottom type):
    var label := (case direction of
      Direction.North => "north",
      Direction.South => "south",
      _ => throw Verify.Unreachable($"unexpected direction: {direction}"));

    // Unreachable branch (statement form):
    throw Verify.Unreachable("control should not reach here");

Note: `throw Verify.Unreachable(msg)` never actually executes `Unreachable` at
runtime if the branch is genuinely unreachable — the `throw` propagates the
constructed exception before any code after it runs.  If control somehow does
reach this line, `Unreachable()` constructs the exception and the `throw` raises it.
```
