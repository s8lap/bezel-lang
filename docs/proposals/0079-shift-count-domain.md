# Proposal 0079 — Shift counts: out-of-range counts fault instead of silently masking

| | |
|---|---|
| **Status** | graduated-v0.242 — superseded — graduated v0.242 (owner-ruled 2026-08-11, all Qs as recommended) |
| **Tracking issue** | [#3045](https://github.com/s8lap/bezel-lang/issues/3045) |
| **Target module(s)** | **spec** — §6.5 (the rule change), §6.6 (`<<=`/`>>=` inherit; one sentence), §17.14 (the constant-evaluation list: "shift-count masking" becomes the shift-count fault), §31.4 (no new type under the Q2 recommendation), Appendix C.1 (invariant row). **stdlib** — none (no member's contract mentions shift counts; proposal 0008's `BitOperations` contract already says "defined faults" and aligns). **rationale** — a new decision-record subsection; §14.6 untouched. **guides** — none (no guide teaches shifting). |
| **Written against** | spec/stdlib v0.241 |
| **Origin** | The 2026-08-11 elegance review (front-half critic, finding 2 — high confidence on the inconsistency, medium on remedy details; coordinator Tier 1 item 2), owner-accepted. |

*Reference convention: unprefixed §-references are language spec references; stdlib and rationale references are marked.*

---

## 0. Thesis

§6.5 conflates two different things under "a shift never raises `OverflowException` (it is a bit operation)":

1. **Bits shifted out of the left operand are discarded.** This *is* the operation — `byte(0b1000_0001) << 1` dropping the top bit is what a shift means, exactly as `&` discards bits. Correct, silent, and untouched by this proposal.
2. **A count outside `0..width-1` is silently reduced modulo the width.** `1 << 33` on `int` computes `1 << 1 = 2`; a count of `-1` masks to 31. This is not the operation — it is an undefined-input rescue inherited from C-family hardware behavior, and it is the only place in the language where an arithmetic-shaped operator receives an out-of-domain operand and silently computes something else. `a + b` overflowing raises (§6.1); `a / 0` raises (§6.1); `byte(300)` raises (§5.2); `1 << 33` returns 2.

**Proposed rule:** the shift count's domain is `0..width-1` of the **left operand's type** (the width §6.5 already keys on, v0.154/#1094). A count outside the domain is a fault — a **compile error** when the count is a §17.14 constant (the §17.14 posture: constant evaluation faults are compile errors at the expression), a runtime fault otherwise. Discarding shifted-out bits stays silent. Deliberate wraparound of the count is spelled visibly: `v << (k & 31)`, `v << (k & 63)` — one `&`, and the reader sees the masking instead of having to know it.

## 1. The current rule, and why its defense fails

§6.5 (v0.241): "The shift count is reduced modulo the bit width of the **left (shifted) operand's type** — `& 7` for `byte`/`sbyte`, `& 15` for `short`/`ushort`, `& 31` for the 32-bit types, `& 63` for the 64-bit types. Bits shifted beyond the left operand's width are discarded; a shift never raises `OverflowException` (it is a bit operation, not one of the overflow-checked operators of §6.1)."

The "it is a bit operation" sentence justifies clause 1 (result bits) and is stretched over clause 2 (the count). The count is not a bit pattern being operated on — it is a *quantity* selecting an operation, and quantities out of domain are exactly what the checked posture faults on. The v0.154 changelog (#1094) only settled *which* operand's width masks; negative counts are discussed nowhere in any document (checked: spec, rationale, changelog). Rationale: addressed-weakly, per the review; the decision record this proposal carries is the missing weighing.

Note also §17.14: constant evaluation runs "under the same semantics the expression has at run time — … §6.5 shift-count masking …". So today `const Flags: long := 1 << 64;` folds to `1` silently at compile time — the compiler *knows* the count is out of domain and computes the masked value anyway. Under this proposal that line becomes a compile error, the same posture as `const B := 2147483647 + 1;`.

## 2. The proposed rule text (sketch)

> The right operand of `<<`/`>>` supplies the shift count. Its **domain** is `0` through `width−1`, where width is the bit width of the left operand's type (`0..7` for `byte`/`sbyte`, `0..15` for `short`/`ushort`, `0..31` for the 32-bit types, `0..63` for the 64-bit types). A count outside the domain — negative, or ≥ width — is a fault: a compile error when the count is a compile-time constant (§17.14), and `OverflowException` at runtime otherwise. Within the domain, bits shifted beyond the left operand's width are discarded silently — that is the operation, exactly as `&` discards bits — and the result never faults. Code that wants the modular count deliberately writes it: `v << (k & 31)` (`& 63` for 64-bit operands) — the mask is then in the source, visible and in-domain by construction. (§17.2's mandatory-parenthesization rules do not cover shift-with-bitwise mixes, but `&` binds looser than `<<`, so the parentheses around `k & 31` are required by precedence — the spelling is unambiguous.)

Consequences through the composing rules:

- **§6.6:** `x <<= k` / `x >>= k` desugar to `x := x << k` etc. and inherit the fault; no write-back subtlety (a shift's result already has the left operand's type). One sentence.
- **§17.14:** the evaluation paragraph's "§6.5 shift-count masking" clause is replaced: a shift whose constant count is out of domain is a compile error at the constant expression, joining overflow and division by zero in the fault list. `const Flags: long := (1 << 10) | (1 << 3);` — §17.14's own example — is in-domain and folds as before.
- **§25 operator overloads:** untouched. A user `operator <<` on a record defines its own contract; the domain rule is for the built-in integer shifts (the §6.5 operators), exactly as §6.1's overflow rule is.
- **Diagnostics — the negative-count/negative-operand note.** `>>` already has a sign story for the *operand* (sign-fill on signed types, §6.5). A reader who writes a negative *count* may be reaching for "shift the other way." The constant-count error message says so: `` shift count -1 is outside 0..31 — a negative count does not reverse the shift's direction; use the opposite operator (`>>`), or mask deliberately with `& 31` ``. This is a diagnostic note only; no semantic rule keys on it (Q3 confirms the wording).

## 3. Precedent, verified

Stated from the drafter's own knowledge, checking the review's citation:

- **Rust** — the critic's cite, true with a qualifier: `a << b` with `b >= width` **panics** ("attempt to shift left with overflow") under debug/overflow-checks builds; in release builds without overflow-checks the count is masked. The *checked* semantics — the one Rust considers the true semantics, with masking as the unchecked concession — faults; the explicit spellings are `wrapping_shl`, `checked_shl`, `overflowing_shl`. Bezel, whose §6.1 arithmetic is checked in every build, takes the checked branch of that fork unconditionally — the same relationship Bezel's `+` already has to Rust's `+`.
- **C# / Java** — mask silently (C# ECMA-334: count masked by 31/63), the behavior Bezel inherited. C# cannot change it for compatibility; Bezel has no users yet.
- **Go** — no masking: an over-large count yields 0 (or 0/−1 for arithmetic right shift) as if bits shifted out one at a time; a negative count panics. Confirms mainstream willingness to fault on negative counts.
- **Zig** — the strictest: the count is *typed* at `log2(width)` bits, so an out-of-domain count is a compile-time type error. The right destination, reached by machinery (dedicated integer widths) Bezel does not have; the fault rule is the reachable equivalent.
- **Swift** — `<<` is a "smart shift" (over-shift saturates toward 0/−1, negative counts reverse direction) with a separate masking `&<<`. Notably: Swift also refused silent masking for the default operator, and reserved the masked form for an explicitly-spelled operator — structurally this proposal's `k & 31`, as an operator.

The trend across post-C languages is uniformly *away* from silent masking; they differ only in whether the default faults (Rust-checked, Go-negative, Zig) or defines a total non-masking result (Go, Swift). No surveyed language validates the review's fear case — none was found where silent masking was chosen *on the merits* rather than inherited.

## 4. The fault type (Q2)

- **`OverflowException` — recommended.** §6.1's checked-arithmetic family already means "this operation on these operand values is not representable"; a count of 33 on an `int` shift is an operation the type cannot perform, the same species as `int.MaxValue + 1`. It keeps §31.4's built-in list unchanged, folds into §17.14's existing fault list wording, is what an `ArithmeticException` boundary arm already catches, and matches Rust's vocabulary ("shift with overflow").
- **`ArgumentException`** — rejected: §31.4 and stdlib §1 band 1 make `Argument*` the defect surface of *method* boundaries; an operator has no parameter to name (`ParamName` dangles), and no §6 operator uses it.
- **A dedicated `ArithmeticException` subclass** — the honest counter-candidate: §6.1's own `DivideByZeroException` is precisely an *operand-domain* error with its own type. Declined for economy, as in 0078 §5: no handler plausibly branches shift-domain-vs-overflow, and the built-in list (§31.4's principle) grows only for faults with distinct handling. If the owner prefers taxonomy purity here, the same choice should be made consistently with 0078's Q1 — the two proposals should land with one answer.

## 5. Codegen cost, honestly

Today's implementation **already spends an instruction on every non-constant shift**: LLVM's `shl`/`lshr`/`ashr` are poison for counts ≥ width, so the backend masks the count before shifting (`& 31`/`& 63`; the fix is pinned by `tests/codegen/numeric_shift_nan_char_513_test.cpp` and the #1094 family, which also caught the wrong-width mask for narrow operands). The new rule replaces that AND with one unsigned compare + branch to the existing throw path (`count u>= width → raise`; a single `icmp uge` catches negative and over-large counts at once for a signed count operand after the unsigned reinterpretation — the standard trick, and the branch is perfectly predicted in the no-fault case). Constant counts fold entirely: in-domain constants compile to the bare shift with **zero** checks — cheaper than today's unconditional mask on paths where the count was constant but the mask was emitted anyway. Net: the checked shift costs one compare+branch where today costs one AND; the same cost profile §6.1 accepts on every add.

## 6. Corpus evidence and migration

Greps at v0.241:

- Raw shift-operator counts over `tests/` are not offered: the harness is C++, so most `<<`/`>>` occurrences are stream insertions, and any aggregate number would be noise. The honest inventory is the flip set below, enumerated by grepping for the masking pins themselves — out-of-domain counts and mask arithmetic in the test sources — not by issue tag (the exact `#1094` tag appears in only two files and does not bound the set).
- **The sites that flip are exactly the masking rule's own pins — four files** (grepped for out-of-domain shift counts and mask comments over `tests/`, v0.241 HEAD): `numeric_shift_nan_char_513_test.cpp` (the §6.5 spec-example pins `1 << 32` → 1, `1 << 33` → 2, `8 >> 33` → 4, the `long << 64`/`<< 65` wide-operand pins, and the #1094 byte-width family — `byte(1) << n` with `n := 8`/`9` masking by 7, *variable* counts); `int_arithmetic_test.cpp` (constant-count folds: `1 << 40` → 256, pinned through `[1 << 40]int`'s length, `1024 >> 33` → 512, and `const SZ := 1 << 40`); `compound_assign_test.cpp` (the #1041 `<<=`/`>>=` masking pins, `x <<= 40` → 256 and the `>>=` sibling); and `dict_compound_wider_rhs_2161_test.cpp` (`d["a"] <<= w` with `var w: long := 33L` → 2, "mask 33 & 31 = 1" — variable counts). Under the new rule the constant-count pins become compile errors and invert into sema/lit rejection tests; the variable-count pins (the byte-width and #2161 cases) invert into runtime-fault tests. No other test relies on masking.
- **Explicit masks in the corpus: one** — the codegen fix itself (the backend's own `& 31`), which the new rule *replaces*; no Bezel-source site masks a count today, so no site needs rewriting to keep masking semantics.
- **Docs**: §6.5's mask sentence, §17.14's masking clause, and the two spec examples are the only normative statements; no guide, no stdlib contract, no proposal (0008's `BitOperations` says "defined faults" — already aligned with this posture; its wrapping/rotate members are where deliberate modular bit motion lives when that batch lands).

**Changelog categories.** Newly **rejected**: constant out-of-domain shift counts (`1 << 33`, `x << -1`, `const … := 1 << 64` folds) — previously silently masked. Newly **defined**: runtime out-of-domain counts raise `OverflowException` — previously silently masked. Unchanged: in-domain shifts bit-for-bit, `<<=`/`>>=` in-domain, user `operator <<`/`>>` overloads. Doc-only: the rationale decision record.

## 7. Alternatives considered

1. **Status quo, recorded.** Write the rationale entry defending masking. Declined: the defense would have to argue that `1 << 33` silently yielding 2 serves some purpose overflow-checked `+` does not — no such argument survived drafting; the honest content of the entry would be "C# did it."
2. **Zig-style typed counts** (`log2(width)`-bit count operands). The cleanest end-state but requires dedicated integer widths (`u5`, `u6`) the type system does not have and should not grow for one operator. Declined.
3. **Go/Swift-style total non-masking semantics** (over-shift → 0/−1, negative count reverses or faults). Total and mask-free, but *silently* value-producing for what is almost always a bug — the same objection as masking, with better values. Also splits `<<`'s semantics from the checked-arithmetic family's fault posture for no gain. Declined.
4. **Fault only on negative counts; keep masking for ≥ width.** Halves the inconsistency instead of removing it; no principled line between the halves. Declined.
5. **Warning instead of error for constant out-of-domain counts.** §17.14's posture is that constant-evaluation faults are errors ("never a deferred runtime fault"); a warning would make the shift the one constant fault that defers. Declined.

## 8. Implementation sketch

- **sema** (`src/sema/ConstFold.cpp`, `ExprTypecheck*`): constant folding of shifts gains the domain check (fault → compile error, matching the existing §6.1 overflow-fold errors); the §6.6 compound path needs nothing (desugars through the same fold).
- **codegen** (`src/codegen/`, the #513/#1094 mask sites): replace the emitted count mask with the compare+branch to the existing `OverflowException` throw helper; delete the mask. Constant in-domain counts emit the bare shift.
- **tests** (spec-cited per `tests/README.md`): sema/lit — constant `1 << 32`/`1 << 33`/negative-count rejection citing §6.5 vNext, `<<=` inheritance, const-fold rejection citing §17.14; codegen (`runAot`) — runtime fault on computed out-of-domain counts (positive and negative), in-domain boundary counts (`x << 31`, `x << 0`) unchanged, the `k & 31` idiom's round trip; the inverted #1094/#513 pins.
- **Size**: small — one fold check, one codegen substitution at an already-centralized site, test conversions.

## 9. Questions for the owner

- **Q1** — adopt the domain rule (constant → compile error, runtime → fault; masking spelled `k & 31`/`k & 63`)? Recommended.
- **Q2** — the runtime fault type: **`OverflowException`** (recommended; §4), or a dedicated `ArithmeticException` subclass — answered consistently with proposal 0078's Q1 if both land?
- **Q3** — confirm the negative-count diagnostic note's direction-confusion sentence (§2; wording latitude with the implementation)?

## 10. Interactions

- **Proposal 0078 (#3044):** shares the Q2/Q1 fault-taxonomy decision; land with one answer.
- **Proposal 0086 item 1 (range step validated at construction, #3052):** a **third site** of the Q2/0078-Q1 fault-type ruling — item 1's fault fires at a range *expression*, not a method boundary, so §4's `ArgumentException` rejection (no parameter to name) applies there with the same force; its drafted `ArgumentException` spelling carries a rider folding the choice into this shared ruling.
- **Proposal 0086 item 2 (`mod=`, #3052):** both touch §6.6/§17.2's operator bookkeeping; textual coordination only, no rule overlap.
- **Proposal 0008 (`BitOperations`, accepted wave D1):** `RotateLeft`/`RotateRight` and the wrapping family are the named homes for deliberate modular bit motion; this proposal makes the operator strict while 0008 supplies the explicit forms — the same operator-strict/method-explicit split as §6.1 arithmetic vs 0008's `Wrapping*`. 0008's "defined faults" contract is already this posture.
