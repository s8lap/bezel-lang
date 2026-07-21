# Proposal 0058 — Flow narrowing for `readonly` nullable fields

| | |
|---|---|
| **Status** | accepted (owner, 2026-07-21, in-session — the sweep's Tier-2 headline); graduation queued as v0.213 |
| **Tracking issue** | assigned at landing |
| **Target** | Spec §7.4 (the locals-only rule gains the readonly-field extension), §6.1-adjacent rationale (the "never fields" record gains its carve-out). Impl: sema narrowing pass. |
| **Written against** | spec v0.212 |
| **Origin** | 2026-07-21 language-improvement sweep (nullability lens, survivor 1); the v0.201 const-local ruling is the accepted immutable-⇒-narrowable precedent. |

## 1. The rule

Flow narrowing (§7.4) extends to reads of **`readonly` and `static readonly` fields of nullable type**, keyed on the **access path** (path root + field):

- Admitted roots: `this` (never reassigned), a type name (static readonly), and a narrowing-tracked local (uncaptured or under §7.4's existing captured-local regime). `obj.F` narrows exactly while `obj`'s own tracking survives; reassigning `obj` invalidates the path's facts — the field's facts ride the root's.
- **A readonly-field fact is never invalidated by calls, `await`s, or closures.** §6.1's invalidators are all *writers*; a `readonly` field has none after construction, through any alias, on any path — that is the whole justification, and it is the same justification the const-local ruling accepted at v0.201.
- **The construction window is excluded**: inside the declaring type's constructors and field initializers (where §22.7 admits assignment to `readonly` fields), the rule does not apply — fields there follow today's no-narrowing rule.
- FFI foreign-writer residual: unchanged (§30.12's recorded residual covers all memory, narrowed or not).

## 2. What it deletes

The single most-taught nullability workaround: the copy-to-local dance for injected optional dependencies (`var log := _log; if log <> null then log.Write(…); end;` — Guide 2 §1's own example class). Service code — Bezel's target domain — holds exactly this shape (`readonly _log: ILogger?`, `readonly _cache: Cache?`) in nearly every class.

## 3. What it does not touch

Mutable fields (blunt rule stands, §6.1's reasons intact); properties (accessor calls, not storage reads); `readonly` fields of record type containing nullable members (the field itself must be the nullable thing); narrowing invalidation for the path *root* (unchanged §7.4 regime).

## 4. Migration

Purely permissive — every program that compiled keeps compiling and no narrowed read changes value. Impl tracker filed at landing.

## 5. Declined shapes

Whole-object immutability inference (narrow any field of a never-mutated object) — needs alias analysis the language deliberately avoids; the keyword is the boundary. Narrowing through property getters — §6.1's TOCTOU reasoning applies fully (a getter may compute).
