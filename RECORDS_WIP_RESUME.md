# Records (CG29 / #241) — WIP resume notes

Branch: `cg/cg29-records-basics` (off main 9801fed). This file is scratch; delete before the final PR.

## State (last verified clean: commit e0c2bba)
- Sema layer DONE & committed (e0c2bba, pushed): `FieldSymbol::posParam`, positional-field symbolization in `SymbolCollection.cpp`.
- Suite at e0c2bba: **1552 passed, 6 failed** (only the 6 record_basics tests), build clean, zero regressions.
- UNCOMMITTED edit (made after e0c2bba, additive-safe): `storageType()` in LlvmBackend.cpp now handles a `FieldSymbol` with `posParam` → `lowerType(fs->posParam->type.get())`. NOT yet built/verified (output channel went dark).

## Key facts (cross-verified)
- `TypeRef::declaration` is a `sema::TypeSymbol*` (TypeRef.hpp:50). So `lowerTypeRef`'s Named branch can call `recordStruct(rt->declaration)` directly.
- `lowerTypeRef` Named branch (LlvmBackend ~461-477): `case Named: if (decl && (Class||Interface)) return ptrTy(); return i32(); default: return i32();`
- `classStruct(TypeSymbol*)` @632: slot0 = vtable ptr, then data fields; `fieldIndex_[m]=idx`.
- `storageType` @600, `isStaticField(FieldSymbol*)` @564, `fieldAddr` @2848, `lowerCall` @3001 (record ctor hook: when `calleeSym` is `TypeSymbol` kind==Record), field-read path @2399.

## Remaining codegen edits (verify each via `./scripts/run-tests.sh`)
1. **recordStruct(TypeSymbol*)** — new helper, like classStruct but NO vtable slot0; `fieldIndex_` from 0 over non-static `FieldSymbol` members (skip methods/props). Cache in a `structs_`-like map (records can reuse `structs_` keyed on the TypeSymbol).
2. **lowerTypeRef Named** — record decl → `return recordStruct(rt->declaration);` (BY VALUE struct, not ptr).
3. **lowerCall** @3001 — `if (auto* ts = dynamic_cast<sema::TypeSymbol*>(calleeSym); ts && ts->kind==Record)` → construct: `alloca recordStruct`, store each positional arg into its GEP (args are positional; match to member order), then `CreateLoad` the struct and return the value.
4. **field read `p.x`** — record value is a struct VALUE, not a pointer. `fieldAddr` does `CreateStructGEP(obj)` where `obj = lowerExpr(target)`. For records, need the target's ADDRESS:
   - record-typed local → its alloca slot (locals_ / lvalueAddr).
   - returned-record temp (e.g. `Make().x`) → materialize into a temp alloca, then GEP.
   - record method `this` (test #4 `Vec.Len2`) → pass record by hidden pointer as `this`; field reads inside the body GEP that pointer.
   Simplest uniform approach: keep record locals as `alloca` of the struct type (already how locals work), and make field access on a record use the lvalue-address path (GEP the slot) rather than ExtractValue on a loaded value.

## Tests (tests/codegen/record_basics_test.cpp) — expected exit codes
construct_and_read=34, passed_to_method=12, returned_from_method=11, body_method_reads_field=25, three_field=123, local_reassigned=89.

## Gotcha
- Output channel intermittently returns EMPTY or drops/fabricates lines. grep -c (token count) is the reliable signal. Verify suite via: `./scripts/run-tests.sh > /tmp/x.log 2>&1; grep -c 'error:' /tmp/x.log; grep -aoE '[0-9]+ passed, [0-9]+ failed, [0-9]+ total' /tmp/x.log | tail -1`.
- Before resuming: `git diff --stat` — expect only LlvmBackend.cpp uncommitted (the storageType edit). If it looks corrupted, `git checkout` it and redo from this file.
