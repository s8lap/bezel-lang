# The imported-symbol seam (#3152 / #3149) — programme status and handoff

*Written 2026-08-14 so a later run can resume without re-deriving anything. Everything
below is measured unless it says otherwise. Where a claim is reading-derived it says so —
this programme's own experience is that **the audits were reliably right that something
was broken and reliably wrong about the mechanism**.*

---

## 1. The defect, in one paragraph

`ModuleLoader` never assigns `Symbol::decl` for an imported symbol. There is no
`ast::TypeDecl` / `ast::MethodDecl` / `ast::PropertyDecl` to assign — the importing
compilation has no AST for a dependency's types at all. So every classification written as

```cpp
dynamic_cast<const ast::TypeDecl*>(sym->decl)      // null for EVERY import
```

is not a null-dereference risk (each site guards) but a **silent answer change**: the
guard's `false` branch is written as a substantive claim — "not a positional record", "no
`serialized` clause", "not borrow-bearing", "does not return owned" — and each of those is
simply wrong across a module boundary, with no diagnostic.

**Two halves.** `TypeSymbol::decl` (umbrella **#3152**) is DONE. `MemberSymbol::decl`
(umbrella **#3149**) is in progress: the loader sets it only where it re-parsed body text
for a GENERIC type, so for a non-generic imported class every member symbol has
`decl == nullptr`, and `PropertySymbol::decl` is **never** set.

---

## 2. Where it stands

| | |
|---|---|
| `origin/main` | `ec6a02f` |
| **class 1** (`TypeDecl` symbol-side) | **152 → 51** ✅ all 12 families landed |
| **class 2** (always-null `TypeSymbol`→`TypeDecl`) | **1 → 0** ✅ (#3150) |
| **class 3** (member-kind symbol-side) | **301**, newly ratcheted, ~41 of 110 UNIFY sites converted |

`scripts/check_imported_symbol_reads.py` enforces all three as a **gate phase**. The
baseline is **EXACT, not a ceiling**: a rise fails, and so does stale headroom, so a
landing family cannot silently hand back what an earlier one ratcheted away.

### Landed (class 1, all pushed)

F5 #3142 · F10 #3145 · F6 #3143 · Sendability · F7 #3132 · F11/F12/F14 #3147 ·
F4 #3140 · F13 #3146 · F3 #3141 · F1 #3139 · F2 #3140 · F9 #3144

### Landed (class 3)

M1 owned-formals #3176 (+ #3186) · M3 auto-property #3152

---

## 3. The plan, in order

**Wave B1 — DONE.** Ratchet extended to member casts (`53020a2`). Deliberately sequenced
*before* the remaining conversions: certifying finished work is the lesser half of what a
ratchet is for. Three families had already landed unmeasured because the tool watched
`ast::TypeDecl` only.

**Wave A — in flight.** M13 (conformance witness, 1 site, FALSE ACCEPT/soundness) + M6
(imported-ABI function maps, 9 sites, CRASH — indirect call to `0x0`); and M2 (owned-return
channel, 15 sites, the widest silent leak).

**Wave D0 — in flight.** The blockers **#3162** and **#2884**. They make M7 (9) and M14 (2)
untestable, and gate #3183 / #3164 / #3179 / #3170. The lane was asked to establish the
true dependency graph by measurement, because `#3164-before-#3162` is a *claim*.

**Wave B2 — next.** M4 (13, silent wrong data, #3177) · M5 (8, false rejects, #3178) ·
M8 (2, purity false accept, #3174). Fold in **#3188** and **#3189**.
⚠ **M5 carries a free win**: `MetaParam::hasDefault` is ALREADY on the wire and simply
dropped by `ModuleLoader::convertParams` — no format change needed.

**Wave C.** M11 const folding (6, OOB via malformed element GEP) · M9 initializers not
replayed (3).

**Wave D.** M7 (9) and M14 (2), once D0 lifts their blockers.

**Wave E — the schema tail.** Six facts genuinely need new wire fields. One —
a **static** property's Auto body form — is *deliberately destroyed*:
`ModuleMetadataBuilder.cpp:1116-1118` force-clears `MetaMember::isAutoProperty` for any
`static` member. The `base(...)` chain-call argument texts are genuinely NEEDS-AST.

**Wave F — the part that is not about imports at all.** The MemberSymbol audit's §6 lists
**fifteen drifted pairs**, **seven comments asserting invariants their own code violates**,
and three dead channels (`contractRecords` is written twice and read nowhere). Same defect
class, wider blast radius, currently unowned.

---

## 4. Rules that were earned the hard way

1. **VERIFY FIRST — and expect the audit to be wrong about HOW.** Five severity readings
   and six site verdicts were corrected by measurement (#3168). Examples: #3132 was rated
   "MEDIUM if live, LOW if latent" and is a **hard compile failure** (two distinct
   malformed-IR shapes); F9's "leak" is really a silent **ABI mismatch** first, the leak
   second; F2's "silent wrong data" was **unreachable as stated**; F3's predicted
   calloc-zero is a loud **false reject**. **Refuting the premise is a success.**
2. **⚠ WHEN YOU REPLACE N NARROW PREDICATES WITH ONE BROAD ONE, ENUMERATE WHAT EACH OLD
   COPY EXCLUDED AND DIFF IT AGAINST WHAT THE NEW ONE ADMITS.** This single question, asked
   of one lane, found **four widenings beyond the three it had already guarded** — one inert
   *only* because of a spec bound the spec itself labels **"liftable"**. A missing guard is
   a silent soundness widening, strictly worse than the bug being fixed. It is the natural
   failure mode of any unification programme and no test can catch it.
3. **A family lands WHOLE or not at all.** #1462 partially unified this exact seam, turned
   a leak into a SIGSEGV, and was reverted.
4. **Check reachability BEFORE writing a test.** Several top-severity codegen sites sit
   behind existing loud rejects; two audit probes died on those diagnostics before reaching
   the site under test. A test that cannot fail is worse than none.
5. **Every case needs a character-identical LOCAL twin, and the set needs an
   OVER-REJECTION control.** Two lanes shipped false rejects because their tests only
   pinned what should be rejected. A test set that would pass if you rejected everything is
   not a test set.
6. **Do NOT add a side table.** Eleven exist, each added by its own bug, each fixing one
   consumer and none making the next correct.
7. **Wire format: distinct record code, never a positional scalar.** #3139's 8th
   `REC_SCALARS` entry is *conditional*, so `sc[7]` means "nBuiltinIfaces" only when it was
   emitted; an unconditional 9th would be read as that count and the decoder would try to
   read that many blocks — **a silent misparse of an artifact**. Warnings are at both the
   emit and read sites. Emission and the `hashTypeDecl` fold must be conditional on the
   **same** predicate: two commits got that wrong, and an unconditional `hashStr` perturbed
   the header hash for modules not using the feature at all.
8. **Prove wire compatibility empirically**, both directions, over shapes that stress
   trailing decode. Do not assert "additive, old readers skip it".

---

## 5. Traps that cost time

- **Both oracles, always.** ASan is a *different* oracle, not a stronger one (#2633). A
  batch passed plain and failed ASan on a **stack-buffer-overflow**; the overflowing write
  landed in stack nothing subsequently read, so nothing faulted.
- **`died on` lines are last-RUN-line MISATTRIBUTION.** The authoritative lines are the
  arbitration's `REAL:` / `FLAKY:`. Chasing the prominent lines cost a build.
- **A gate validates the WORKING TREE but pushes HEAD.** `gate_and_push.sh` now refuses a
  dirty tree; the tree is frozen during a gate, `scripts/` included (bash reads a script
  lazily by byte offset, and restoring the bytes does not save the run).
- **Wait on IDENTITY, never a process pattern.** `scripts/wait_pid.sh`. Run the gate
  detached (`setsid nohup`) — a harness-tracked job can be killed at its own timeout while
  orphaned shards keep writing to the log.
- **`--area` does NOT narrow the compile** for 5 of 6 areas (#3172): `--area module`
  schedules 2,228 codegen actions against 30 module ones.
- **When a silent-accept hole closes, the tests written DURING it are what break** — four
  instances. They encoded the bug as their expectation. Expect it; do not assume a
  regression.

---

## 6. Open issues, grouped

**Blockers (fix first):** #3162 · #2884 · #3183 (⚠ now established as MEMORY CORRUPTION —
an 8-byte slot taking a 12-byte write) · #3164 · #3170

**Next conversions:** #3177 (M4) · #3178 (M5) · #3174 (M8) · #3188 · #3189 · #3182

**Blocked / sequenced:** #3179 (needs #3162+#3164) · #3185 · #3187 · #3167

**Cross-cutting, not import-specific:** #3169 (unify the three storage enumerations) ·
#3173 (generalize `bindArgsToParams`) · #3171 · #3165 · #3175 · #3160 · #3181 · #3159 ·
#3161 · #3184 · #3168 (the audit's own §5 column is wrong in 6 rows — **do not trust that
column**)

---

## 7. What "done" means

Not "110 → 0". The honest target:

1. every **reachable** UNIFY site converted, or declined **with a filed issue**;
2. the **34 NEEDS-AST** sites documented as permanently exempt;
3. the ratchet covering all three classes, so neither half can regrow.

Three sites are load-bearing **as** decl-casts and must NOT be unified: `Setup.cpp:15482`
is deliberately byte-identical to the lowering's own gate at `Calls2.cpp:7905`, and that
identity is what keeps `eligible ⟹ nulled` exact — unifying one alone converts a safe leak
into a **double free**.
