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
| `origin/main` | `ddf41f2` — **6-commit batch PUSHED**, gate cycle 6 GREEN (plain attempt 8 trustworthy-green, ASan attempt 1 trustworthy-green, conformance 178/0/393) |
| **class 1** (`TypeDecl` symbol-side) | **152 → 51** ✅ all 12 families landed |
| **class 2** (always-null `TypeSymbol`→`TypeDecl`) | **1 → 0** ✅ (#3150) |
| **class 3** (member-kind symbol-side) | **305 → 292** (M4 −7, M8/#3188/#3206/#3182 −3, #3178 tooling −3) |

⚠ The class-3 figure is **305, not the 301** an earlier draft of this table carried: the
baseline was regenerated after Wave A landed and the file is authoritative over this
document. Read `scripts/imported_symbol_reads.baseline`, never a number quoted here.

### #3191 family — landed + follow-up

**#3191 (commit `43d6abc`)**: an imported class's destructor never crossed the wire, so every
imported class was classified trivial-dispose — making §21.9's hard-error never fire cross-module.
Fix added `MetaTypeDecl::nonTrivialDispose` (encoder side) and `TypeSymbol::importedNonTrivialDispose`
(decoder side), reusing the existing inheritance walk.

**⚠ FIRST GATE CAUGHT AN OVER-FIRE on 43d6abc**: `xmod_owned_return_channel_3189.t2` flipped
to FAIL because §21.9's reject was firing on `Cell.MakeIt(i);` (a CALL expression) — but #3189
DOCUMENTED that the call path is reclaimed by `disposeOwnedValue`, not §21.9's hard-error gate.
**This was a width mismatch in the §21.9 walk, not in the carrier.** Root cause: the §21.9
walk fired the reject whenever `cur->importedNonTrivialDispose` was set on the *expression*,
without distinguishing NEW from CALL.

**#3191 follow-up (commit `0bb85f1`)**: gate the §21.9 reject fire on the SOURCE EXPRESSION
being a `NewExpr` (via `dynamic_cast<const ast::NewExpr*>`). CALL expressions always reclaim
through `disposeOwnedValue` per #3189's design intent. T10 test (`t10_new_vs_call_boundary_3191`)
pins the boundary: `new Cell(1)` rejects (NEW → §21.9 hard error); `Cell.MakeIt(1)` runs clean
(CALL → reclaim).

**★ RULE (now earned):** *When you add a wire carrier that makes a §-section's hard-error gate
fire on imported types, the §-section's AST predicate still owns the firing condition. The carrier
only supplies the verdict that predicate would have asked the local symbol for.* In this case,
§21.9's predicate is "the discard is a NEW of a non-trivial-dispose class"; the carrier supplies
"is this imported class non-trivial-dispose?" — the predicate, not the carrier, decides what
counts as a NEW.

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

**Wave A — DONE, reviewed, in the unpushed batch.** M2 (owned-return channel, `06568ef`
+ follow-up `47a31f0`), M6 (`726539b`, #3190), M13 (`843e30f`, #3194), plus the review
follow-up `17fd6a5`. The M6/M13 review `nm`-confirmed the finding that matters most beyond
this programme: `emitExportedProperty/Indexer{Getters,Setters}` **skip virtual members**, so a
virtual accessor body is InternalLinkage and a virtual indexer has no exported symbol at all,
while a virtual method gets an ExternalLinkage `bz$<Class>$<M>`. **#3190, #2837, #3199 and
#3204 are one asymmetry seen from four call sites** — widen that gate once rather than fixing
any of them individually.

**Wave D0 — in flight.** The blockers **#3162** and **#2884**. They make M7 (9) and M14 (2)
untestable, and gate #3183 / #3164 / #3179 / #3170. The lane was asked to establish the
true dependency graph by measurement, because `#3164-before-#3162` is a *claim*.

**Wave B2 — M4 and M8 LANDED (reviewed); M5 still out.** M4 (#3177) landed as five
commits `6d55962..8828f45`; M8 (#3174) + #3188 + #3206 + #3182 landed as `2be3b0c..3f1a4bd`.
M5 (#3178) is the one still in flight. #3189 landed with Wave A.

Two things Wave B2 established that outlive it:

1. **The audit's site counts are approximate and its verdicts are samples.** M4 had 16 sites,
   not 13, and TWO audited rows were refuted by measurement (`LlvmBackend_String.cpp:2596/:2627`
   — the reading is right, the predicted consequence does not occur). M8's lane refuted its
   issue's "opposite direction" section, and then the REVIEW refuted the refutation: it held
   only for constructors with primitive parameters, because a ctor naming a sibling type fails
   the synthetic-source re-parse (#3219). **Two levels of refutation on one claim** is the
   clearest evidence yet for the verify-first rule.
2. **#3188 was filed as "the third copy" and is the FOURTH OF SIX.** The lane measured the two
   it was not told about, both broken identically (0 instead of 211, plus a 16-byte leak), fixed
   them as #3206, and collapsed all six behind two accessors. When an issue says "the Nth copy",
   treat N as a lower bound and go count.
⚠ **M5 carries a free win**: `MetaParam::hasDefault` is ALREADY on the wire and simply
dropped by `ModuleLoader::convertParams` — no format change needed.

**Wave C — LANDED.** M9 initializers not replayed (3 sites) — **#3218** `imported?.ClassTypedField`
drops (classStruct retry on the `?.` GEP lookup mirroring the `.` form's precedent at
`LlvmBackend_Layout.cpp:5799`), **#3209** `imported?.ComputedProperty` drops (parallel
`importedPropGetterFns_` arm in the `?.` member-access lowering, gated on
`!isAutoPropertyStorageSymbol`). **#3223** (sister to #3191, `isTrivialDisposeClass` consults the
carried `importedNonTrivialDispose` verdict on the throw carve-out — the 4th independent copy of
§28.9's classification). **#3219** (decl-less widening of `findUserCtorByArity`, gated on
`matching > 0` precedence). **#3221** (the schema tail: 18th scalar `posParamBodyRef` carried
across the wire so #2364's §9.8 base-with-storage guard fires on an imported base whose storage
is a captured primary-ctor parameter — predicate arm stays narrow: `if (fs->posParam) return
fs->posParamBodyRef;`, NOT widened). 5 commits `7ff67d9..ddf41f2`, all 5 per-fix reviews PASS.

M11 const folding (6, OOB via malformed element GEP) did NOT yet ship; the picks drained
the lower-blocks first because the `?.` family is at risk of being re-broken by every
adjacent change to property lowering. **Wave D** now picks up M11 + the next blocks.

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

## 6b. Live findings from the 2026-08-14 lanes (pre-review, recorded so they are not lost)

**⚠ THE MEMBER AUDIT'S REACHABILITY COLUMN IS WRONG FOR M6, IN BOTH DIRECTIONS** (M6 lane,
measured; under review). The audit says "reachable YES for a non-virtual imported base
indexer; the virtual sub-shape also needs #2837's ABI". Measured: the **non-virtual** shape
is ALREADY CORRECT (#2414/#2449) and the **virtual** shape crashes TODAY with no new ABI —
SIGSEGV 139 on six ordinary spellings. It is also **not indexer-specific**: the
named-property twin at `ensurePropertyAccessorFn`, which Appendix A marks
**ALREADY-CORRECT (guarded)** citing #2387/#2400, crashes identically, because
`declareImportedProperty{Getters,Setters}` skip anything with a vtable slot. Filed #3190.

**`Stmts.cpp:7411` is NOT reachable** — `lowerIEnumerableFor` bails on a null `getEnumDecl`
first, so `for var x in importedColl` is a loud NYI and a test written against that site
today would be **vacuous**. The audit's §2 M15 "inconclusive" note is likewise refuted: the
single-module twin failed for an unrelated reason (#1779), so the boundary IS isolable, and
isolating it found a real crash (#3197).

**★ ONE ASYMMETRY, THREE CALL SITES — the highest-leverage item currently known.**
`emitExportedProperty/Indexer{Getters,Setters}` all skip virtual members, so an accessor
body has **InternalLinkage** in the dependency (`nm`-confirmed) while a virtual method gets
an ExternalLinkage `bz$<Class>$<M>`. **#3190, #2837 and #3199 are that one gate seen from
three places.** Fixing the export gate once would collapse all three, and would change the
sequencing of the remaining waves — worth settling before Wave C/D.

**M2's refutation found a second, wider defect.** Fixing the `owned`-return channel closes
the leak but does NOT make §21.9's diagnostic fire, because **an imported class's destructor
never crosses the wire, so every imported class is classified trivial-dispose** — reachable
with no `owned` anywhere (`new Derived();`, `Derived` local, base imported). Filed #3191.

**M13 is worse than "false accept"**: return-type mismatch → SIGSEGV; param-type mismatch →
**225** from a truncated string pointer; a dropped `owned` return → a 16-byte LSan leak.
§11.2 supplies the third answer the audit's "both directions are wrong" bind needs — reject
the conformance, which is what the local twin already does. Filed #3194.

**Also filed by these lanes:** #3192 (an imported user method named `Select`/`Min`/`Max`
treated as the stdlib intrinsic) · #3193 (method reference over an imported method loses
return type and marker) · #3195 (a plain `var` receiver of a form-2 `scoped` call is never
disposed — single-module) · #3197 · #3199.

## 6c. The blocker dependency graph, MEASURED (2026-08-14, review COMPLETE)

The graph in §6's "Blockers (fix first)" line was a guess. Measured, and every load-bearing
row below was **independently re-measured by the per-fix review** in throwaway worktrees:

- **#3164 → #3162 is REAL and stronger than filed. They are ONE commit.** With
  #3162's halves applied and only #3164's carriage disabled: `new Holder(owned r, 20)`
  is a FALSE REJECT (§22.3 makes the marker mandatory, so the constructor is
  uncallable) and `new Holder(r, 20)` compiles clean and **SIGSEGVs** — #2659's double
  free, one module out, against a twin answering 211. **Shipping #3162 alone is a
  memory-safety REGRESSION, not an incomplete feature.** ✅ **Independently reproduced
  by the review** — it patched only the carriage line
  (`ModuleMetadataBuilder.cpp:1032` → `if (false && …)`), rebuilt `bezelc`, and got
  exactly the three outcomes above (`rejected / SIGSEGV 139 / 211`), against
  `211 / rejected / 211` with the line restored. This is the reason the two issues
  are one commit, and it is the single most important row in this section: a future
  run tempted to split them should read it first.
- **#3183 ⊥ #3162 — the gating claim is REFUTED.** #3183 is live TODAY through
  imported RECORDS, which already construct cross-module; #3162 would only have
  extended it to classes.
- **#3179's stated blocker is partly refuted** — the primary-ctor parameters DO have
  somewhere to ride (§22.8 makes step 0 the caller's, and `bz$<Type>$init` is steps
  1+2 against the object it is handed). Its real remaining work is the §9.8 wire shape
  and tier-4 polymorphic-registry carriage, not the construction ABI.
- **#3162's "ABI gap" framing is wrong** — no new symbol or wire field was needed. It
  was an ORDERING gap.
- **#3164's own record example is not legal Bezel**: `record R(owned h: T)` is a PARSE
  error (Appendix A.3 admits no modifiers on positional fields — §28.5's "records may
  not have `owned` fields", enforced syntactically). It is a class-primary-constructor
  concern only, so its latency claim holds.
- **#2884 is CLOSED as a decline-with-reasons.** The real work is **#2893** (carry
  `propertyBodyText`, ~380 lines mirroring the method-body path, 7 `!pd` fallback
  branches that all flip) — a lane, not a tail. ⚠ There is a **direct contradiction**
  between #3170's measurement and #2943's gate comment (which asserts a one-`T`-slot
  layout does NOT move). Settle it before touching that threshold again: widening it
  to ≥1 was already tried and reverted. **Now sharpened and posted to #3170**: the
  gate's cited evidence ("2894 T1 = 108 with single `_t: T`") **instantiates
  `GIface2894T1<int>`**, where nothing can move regardless of field count — so it
  cannot distinguish "one T field is safe" from "an `int` substitution is safe", and
  it is the second. The review measured a **single**-`T`-field class crashing:
  `G1<int>` → 107, `G1<string>` → **SIGSEGV 139**, twin 107. The real discriminator
  is not how many `T` fields but **whether any field follows one whose substituted
  width/alignment differs from the erased placeholder's** — a type-kind question,
  answerable at sema time without the LLVM data layout the comment says is missing.

**⚠ #3198 (HIGH) is the general form of #3184 and applies to EVERY additive metadata
change this programme made:** stale artifacts have no vintage check, so a metadata
CONTENT change silently reopens the hole it closed — measured as a SIGSEGV. Whoever
picks this up should treat it as a precondition for trusting any cross-version claim,
including the byte-identity results recorded above. The review sharpened this too:
for #3164 the new-reader/old-artifact direction is **strictly worse than the old
compiler** (the old one rejected the shape; the new one compiles it clean into a
SIGSEGV), and there is no lever short of #3198's plumbing — a `kSchemaVersion` bump
does not help (read-older semantics) and the loader **discards the version entirely**,
so no vintage gate is possible today.

## 6d. Filed from the blockers review (2026-08-14)

- **#3205** — an `owned` primary-constructor parameter that is **never referenced** is
  silently dropped and leaks 16 bytes (LSan-confirmed; single-module, so pre-existing).
  §22.10's capture enumeration has three bullets and this is a **fourth case that falls
  off the end** — the "negative-list ⇒ silently do nothing" shape from the 2026-08-06
  taxonomy. Filed as a **spec question first** (the section's own words say silently
  dropping it "would be a leak"), with a recommendation to capture rather than reject.
- Nits folded into the batch rather than filed: #3164's widening enumeration was short
  by two readers (`TypeResolution_Names.cpp:66`, `Sendability.cpp:211` — both inert, but
  *not* by the mechanism the commit stated); #3183's consumer list undercounted by one
  measured case (`.Length` on an imported inline array was an `internal:` error reported
  **alongside exit 0**, now 4); and #3164 **falsified a comment in `Sendability.cpp`**
  that asserted `FieldSymbol::modifiers` is never populated for a positional field —
  true when written, false the moment the carriage landed.

**Also filed by that lane:** #3196 · #3200 · #3201 (the generic half of #3162, declined
rather than shipped silently zeroed) · #3202.

## 7. What "done" means

Not "110 → 0". The honest target:

1. every **reachable** UNIFY site converted, or declined **with a filed issue**;
2. the **34 NEEDS-AST** sites documented as permanently exempt;
3. the ratchet covering all three classes, so neither half can regrow.

Three sites are load-bearing **as** decl-casts and must NOT be unified: `Setup.cpp:15482`
is deliberately byte-identical to the lowering's own gate at `Calls2.cpp:7905`, and that
identity is what keeps `eligible ⟹ nulled` exact — unifying one alone converts a safe leak
into a **double free**.
