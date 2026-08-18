# Self-Hosting Bezel Standard Library Collections — Design Proposal

**Status:** under-review — Draft proposal for owner review. No source changes; this document only.  
**Scope:** Moving the C++ runtime implementations of the stdlib collections into
`.bzl` source that the Bezel compiler compiles, so the compiler's hardcoded
"name == Array" gates progressively atrophy.  
**Context:** Issue #545 direction; rationale §5.11 (v0.97) and §5.20 (v0.132).  
**Provenance:** Drafted mid-June 2026; committed 2026-07-04 as proposal 0005 (it had
sat untracked at the repo root — the custody gap the coverage audit flagged). Written
pre-v0.168 Owning-rename and pre-#1414-stage-3 registry; the mechanical refresh
recorded in the provenance note below has since brought its names and anchors current.

---

> **Provenance note — mechanical refresh performed 2026-07-30 (owner-directed
> 2026-07-29).** The pre-v0.168 type names this draft was written with
> (`OwnedStack` / `OwnedBuffer` / `OwnedArray` / `OwnedQueue` / `IOwnedCollection`, …)
> were renamed to the current `Owning*` / `IOwning*` family (stdlib v0.231; spec
> §14.4); every `src/` file:line anchor was re-verified against the current tree
> and updated (several cited mechanisms have moved or been superseded — notably
> the collection-name predicates now read the #1414 stage-3 registry in
> `src/common/BuiltinRegistry.cpp`, and the former monolithic
> `bezel_runtime.cpp` was split into per-domain `bezel_runtime_*.cpp` files);
> and the §5 POC's conformance claim was corrected for the v0.222
> `OwningStack`/`OwningQueue` demotion (#1811). The architecture and slice plan
> were not changed.

## 1. Current State

### 1.1 C++ Runtime Implementations

Every stdlib collection is implemented in
`src/runtime/bezel_runtime_collections.cpp` (~2,980 lines; the former monolithic
`bezel_runtime.cpp` has been split into per-domain `bezel_runtime_*.cpp` files)
as `extern "C"` functions the LLVM backend calls directly.

**`bz_array_t`** (struct in `bezel_runtime_internal.hpp:94–108`) backs `Array<T>`,
`OwningArray<T>`, `Stack<T>`, `OwningStack<T>`, `Queue<T>`, `OwningQueue<T>`,
`Set<T>`, `OwningSet<T>`, and `OwningBuffer<T>`.  Key functions
(`bezel_runtime_collections.cpp` lines 10–222; the LIFO drain helper at 1191):

| Function | Serves |
|---|---|
| `bz_array_alloc` | `new Array<T>()`, `new Stack<T>()`, etc. |
| `bz_array_add` | `xs.Add(x)`, `xs.Push(x)`, `xs.Enqueue(x)` |
| `bz_array_get` / `bz_array_set` | `xs[i]` get/set (value elements) |
| `bz_array_set_disposing` | `xs[i] := v` (class elements, OwningArray) |
| `bz_array_free` | scope-end `Dispose()`, class elements reversed (§28.11) |
| `bz_array_mark_disposed` | checked-disposal quarantine (§28.2/§621) |
| `bz_array_pop_at` | `OwningArray.PopAt(i)` — transfer ownership without disposal |
| `bz_array_clear_disposing` | `OwningArray.Clear()` |
| `bz_array_drain_next_lifo` | Stack drain (LIFO order) |

**`bz_dict_*`** (lines 320–1163) backs `Dictionary<K,V>` and
`OwningDictionary<K,V>`.  Includes separate string-key (`bz_dict_*_str`) and
generic-key (`bz_dict_*_gen`) variants because Dictionary key hashing is
runtime-polymorphic.

**`bz_llist_*`** (lines 1330–1601) backs `LinkedList<T>` and
`OwningLinkedList<T>`.  Uses a separate doubly-linked node struct.

**`bz_buffer_*`** (lines 2931–2978) backs `Buffer<T>` (value-element fixed-
capacity primitive, issue #545).  Three functions only: `bz_buffer_new`,
`bz_buffer_get_ptr`, `bz_buffer_free`.

**`OwningBuffer<T>`** with class-typed T is *not* `bz_buffer_*`; it reuses
`bz_array_t` (see `bz_array_alloc_count_n`, `bezel_runtime_collections.cpp:31`;
confirmed in `LlvmBackend_Exprs2.cpp:2743–2760`).

### 1.2 Codegen Lowering — How `new Array<T>()`, `xs.Add(x)`, `xs[i]` are Lowered

**Construction** — `LlvmBackend_Exprs2.cpp` lines 2611–3010 (the dispatch moved
out of `LlvmBackend_Exprs.cpp`): a name-switch on `colName` (line 2613) dispatches
each collection type to the appropriate runtime allocator.
`Stack<T>`, `OwningStack<T>`, `Queue<T>`, `OwningQueue<T>`, `Set<T>`, `OwningSet<T>`,
`Array<T>`, `OwningArray<T>` all reach `bz_array_alloc` (line 2918);
`OwningBuffer<T>(n)` reaches `bz_array_alloc_count_n` (line 2756).
`LinkedList<T>` reaches `bz_llist_alloc` (line 2730).
`Dictionary<K,V>` reaches `bz_dict_alloc` or `bz_dict_alloc_generic` (lines 2985–2990).

**Method calls** — `LlvmBackend_Calls.cpp` `tryLowerCollectionInstanceCall`
(entry at line 681). The function opens with the type-name predicate calls
(`isCollectionTypeName`/`isDictTypeName` at lines 701–702, `isLinkedListTypeName`
at line 751), then dispatches on method name strings (`"Add"`, `"Push"`,
`"Enqueue"` at line 3954; `"Pop"`, `"Dequeue"` at line 4507; etc.).

**Indexer get/set** — `LlvmBackend_Assign.cpp`: `lowerCollectionIndexerSet`
(line 655) and `lowerCollectionIndexerGet` (line 164; it superseded the former
`tryLowerIndexExpr` in `LlvmBackend_Exprs.cpp`) dispatch on the same type-name
predicates.

### 1.3 Quantified "Hardcoded Collection Name" Surface

A grep for `"Array"`, `"OwningArray"`, `"Dictionary"`, `"OwningDictionary"`,
`"Stack"`, `"OwningStack"`, `"Queue"`, `"OwningQueue"`, `"Set"`, `"OwningSet"`,
`"LinkedList"`, `"OwningLinkedList"`, `"OwningBuffer"`, `"Buffer"` across
`src/` (excluding `.h`/`.hpp` headers and test/ directories) yields **~385 sites**
(re-counted 2026-07-30; the surface has grown since drafting):

| Subsystem | Approximate count |
|---|---|
| `src/sema/` | ~194 |
| `src/codegen/` | ~137 |
| `src/ownership/` | ~38 |
| `src/common/` (the #1414 stage-3 registry) | ~15 |

Representative clusters (the ones a self-host migration would have to displace):

- **`TypeResolution_Names.cpp:44–296`** — `builtinTypeArity`: the master list of
  which names are built-in generics (collection entries at lines 137–190). Stack,
  Queue, Set, LinkedList, OwningBuffer are listed here; removing them from this
  list requires they resolve as ordinary imported TypeSymbols instead.
- **`LlvmBackend_Calls.cpp:397–427`** — `isCollectionTypeName`,
  `isLinkedListTypeName`, `isDictTypeName` predicates that gate every method-call
  lowering path. The name sets themselves now live in the #1414 stage-3 registry
  (`src/common/BuiltinRegistry.cpp:69–87` — `kArrayBackedCollections` /
  `kDictionaryNames` / `kLinkedListNames`), which superseded the predicates'
  former inline hand-lists.
- **`LlvmBackend_Exprs2.cpp:2611–3010`** — the `new Coll<T>()` dispatch switch.
- **`ExprTypecheck_Exprs.cpp:914–919, 1487–1519, 1996–2001`** — sema checks that
  resolve member types and indexer validity based on literal collection name
  strings.
- **`LlvmBackend_Disposal.cpp:415`** — `emitOwnedFieldDisposals` (moved out of
  `LlvmBackend_Setup.cpp`): the owned-field disposal loop that identifies a field
  as a "collection" (hence requiring `bz_array_free` / `bz_llist_free`) via
  `ownedCollectionFieldType` (`LlvmBackend_impl.hpp:5498`), which applies
  `isCollectionTypeName`/`isLinkedListTypeName`/`isDictTypeName`.
  **This is the deepest coupling**: a self-hosted collection FIELD in another
  class must still trigger the correct destructor sequence at the field owner's
  Dispose time. If the collection is a user class, the standard vtable-dispatch
  path handles this automatically — this is actually a simplification.

---

## 2. Foundation Already in Place (#545)

### 2.1 Element-Policy Unification

`src/codegen/LlvmBackend_ElementPolicy.cpp` (280 lines) implements
`Emitter::elementPolicyFor(elemTy, owningFamily)` — the §28.11 v0.135 table
translated into code (line 72). This factory returns an `ElementPolicy` struct
whose `disposerThunk` field is the correct LLVM thunk for any element category
(ClassOwned, ClassBorrow, RuntimeManaged, Value), derived from the element type
alone rather than from per-collection name switches.

This unification (issues #602, #619, #545 batch 4) means that any new collection
type — whether intrinsic or self-hosted — automatically gets the right per-element
lifecycle if it calls `elementPolicyFor` or if its disposal flows through the
vtable (which standard user classes do).

### 2.2 `Buffer<T>` Primitive (Value-Element Fixed-Capacity)

`TypeResolution_Names.cpp:233`: `Buffer<T>` registered with arity 1.
`src/runtime/bezel_runtime_collections.cpp` lines 2931–2978: three C functions
(`bz_buffer_new`, `bz_buffer_get_ptr`, `bz_buffer_free`).
`LlvmBackend_Exprs2.cpp` lines 3289–3335: `new Buffer<T>(n)` lowered to
`bz_buffer_new(len, elem_size)`.

`Buffer<T>` is restricted to primitive value types (int/byte/short/long/double/
decimal/char/bool) per the `TypeResolution_Names.cpp:230–232` comment and the
sema enforcement at `ExprTypecheck_Exprs.cpp:2380–2405` (which superseded the
comment-only note formerly cited in `LlvmBackend_impl.hpp`). Class-element
`Buffer<T>` is the §5.20 deferred design pass.

### 2.3 `OwningBuffer<T>` with Class T

Per spec stdlib §4.14 and the `OwningArray<T>` reference implementation at
`tests/conformance/cases/stdlib_0039.bzl`, `OwningBuffer<T>` with class T is the
intended storage primitive for every self-hosted owning collection.

The compiler currently lowers `OwningBuffer<T>` (class T case) as a `bz_array_t*`
with `bz_array_alloc_count_n` (construction), `bz_array_get/set/set_disposing`
(indexer), and `bz_array_free` with the element disposer from `elementPolicyFor`
(disposal). The `move buf[i]` / field-restoration pattern that the reference
implementation uses is exercised by `generic_owned_field_disposal_test.cpp` (the
`Box<T>` test, lines 145–166) and the move-interface-slot tests in
`tests/ownership/`.

**Conclusion:** `OwningBuffer<T>` for class T is implemented and tested at
the intrinsic level. The stdlib reference implementation of `OwningArray<T>`
(stdlib §4.5 and `tests/conformance/cases/stdlib_0039.bzl`) is purely
expressed in Bezel source and is the template for self-hosting. The compiler
already understands every operation that source uses.

---

## 3. The Prelude-Bootstrap Gap

### 3.1 How Stdlib Types Are Registered Today

There is **no `.bzl` prelude**. Every stdlib type is registered as a builtin by
name inside `builtinTypeArity` (`TypeResolution_Names.cpp:44–296`). A type not in
that function gets `kNotBuiltin` (line 296) and causes an "unresolved type name"
diagnostic. (The `injectStdlibPrelude` hook that now exists at `Driver.cpp:80` is
not such a prelude — it injects one synthesized AST declaration, `Result<T, E>`
(#2047), not a compiled `.bzl` file.)

The `TypeRef` for a builtin resolves with `Kind::Named`, `declaration = nullptr`
(no `TypeSymbol`), and an `astNode` pointing at the AST type reference node. All
codegen predicates (`isCollectionTypeName`, `isLinkedListTypeName`, etc.) check
`rt->declaration == nullptr` **plus** the string name before routing to the
runtime call path. A user-defined class would have `rt->declaration != nullptr`,
causing all those predicates to return false and the code to fall through to the
standard class dispatch.

### 3.2 Designing the Prelude-Bootstrap Loader

**Goal:** Before compiling user code, parse and typecheck a stdlib prelude
(one or more `.bzl` files containing the collection class definitions), register
their `TypeSymbol*` into `SemaContext`, and let codegen handle them via the
standard user-class path.

**Hook point:** `Driver::compileFile` (`src/driver/Driver.cpp` lines 165–339;
thin forwarding overloads at 153/159). The pipeline today is:

```
readFile → Lexer → Parser → injectStdlibPrelude → collectSymbols
  → (loadDependencies) → resolveTypes → typecheckExpressions → checkPureBodies
  → checkDefiniteAssignment → checkStaticInitCycles → checkOwnership → codegen
```

The prelude loader would run between `SemaContext semaCtx(diagnostics)`
(line 203) and `collectSymbols(*unit, semaCtx)` (line 204) — or equivalently, as
the first step of `collectSymbols`, loading the prelude's symbols before the user
unit's symbols so user code can reference them.

**Mechanism:** Reuse the existing cross-module loader infrastructure
(`module::loadDependency` / the batched `loadDependencies`,
`ModuleLoader.cpp:1523/1546`). The stdlib prelude is compiled
once to an object + metadata (the `.bezel.meta` section) and loaded as a special
"always-present" dependency. The `SemaContext::importedByModule_` table
(`SemaContext.hpp:382`; registration/lookup at lines 201–239) would map a
`"System"` or `"System.Collections"` module
name to the prelude's exported `TypeSymbol*` objects.

**What changes in name resolution:** `TypeResolution_Names.cpp`'s
`builtinTypeArity` currently returns a nonzero arity for collection names so they
don't fail "unresolved type." Once a type's `TypeSymbol*` exists in the sema
context (from the prelude), `resolveTypes` finds it via the normal symbol-lookup
path, `builtinTypeArity` is not consulted, and `rt->declaration != nullptr` makes
every `isCollectionTypeName`/etc. predicate return false. The collection then
routes through the standard user-class codegen path.

**Incremental approach:** Remove each type from `builtinTypeArity` only after
its prelude `.bzl` file is loaded. This lets the existing and the self-hosted
paths coexist during migration, type by type.

**Owner decision required (D1):** Where does the prelude `.bzl` live in the
repository? Options: (a) `src/stdlib/` compiled as a separate target and linked
in, (b) embedded as a binary blob in the compiler binary (reproducible but
verbose), (c) shipped alongside the compiler executable and loaded from disk.
Option (a) is the cleanest; the cross-module loader already handles this workflow.

---

## 4. The Runtime-Sized Owning-Buffer Gap

### 4.1 The Problem

A user-written Bezel class **cannot today hold runtime-sized owning storage
as a field** without `OwningBuffer<T>`. But `OwningBuffer<T>` is itself a
compiler intrinsic — it has no user `TypeSymbol`; its construction, indexer,
`move buf[i]`, and disposal are all hardcoded in the backend.

The reference implementation of `OwningArray<T>` (stdlib §4.5) uses
`OwningBuffer<T>` as its backing field. A self-hosted `OwningStack<T>` would do the
same: `private owned _storage: OwningBuffer<T>`. That is **already valid Bezel
source** because:

1. `OwningBuffer<T>` is a compiler-known type — `builtinTypeArity` returns 1
   for it (line 183 of `TypeResolution_Names.cpp`).
2. Its owned field in a user class triggers `ownedCollectionFieldType` in
   `LlvmBackend_impl.hpp:5498`, which returns the TypeRef so
   `emitOwnedFieldDisposals` (`LlvmBackend_Disposal.cpp:415`; moved out of
   Setup.cpp) calls `bz_array_free` with
   the correct element disposer from `elementPolicyFor`.
3. The `move buf[i]` and `buf[i] := value` indexer operations on an
   `OwningBuffer<T>` field are lowered by the existing intrinsic paths in
   `LlvmBackend_Assign.cpp` (indexer get/set/move) and `LlvmBackend_Exprs2.cpp`
   (construction).

**`OwningBuffer<T>` as the owned-storage primitive is NOT a gap.** The spec
already designates it as the intrinsic boundary (rationale §5.14, §5.15,
§5.20). After v0.99, "the intrinsic surface of the entire owning-collection
family reduces to one well-scoped primitive (`OwningBuffer<T>`)" (rationale §5.14).

### 4.2 Options for Any Remaining Gap

The **only residual gap** is that `OwningBuffer<T>` itself remains a compiler
intrinsic — users cannot rewrite it. The spec explicitly accepts this (stdlib
§4.14: "Why this is a compiler-known primitive"). If the goal is to self-host the
*collections that compose over* `OwningBuffer<T>`, no additional primitive is
needed.

If in the future even `OwningBuffer<T>` must be expressible in Bezel source, the
options are:

- **(a) Expose `OwningBuffer<T>` as a full user TypeSymbol** with a compiler-
  synthesized body for its one intrinsic operation (runtime-sized heap allocation).
  Compiler knows the allocation; everything else (indexer, move, disposal) is
  already standard user-class machinery. Risk: moderate — requires extending the
  prelude bootstrap to handle the one constructor that needs the intrinsic
  allocation call.
- **(b) A new `HeapArray<T>` primitive** at the FFI layer (`extern class`). Risk:
  requires spec amendment (out of scope for implementation PRs per CLAUDE.md).
- **(c) Defer to the §5.20 design pass** — the spec explicitly defers class-
  element `Buffer<T>` and the borrow-storing primitive to future design work.
  Accept `OwningBuffer<T>` as the permanent intrinsic floor.

**Recommendation:** Option (c). Keep `OwningBuffer<T>` as the intrinsic floor
and self-host only the collections that compose over it. This matches the spec's
own stated intent and avoids a spec amendment.

**Owner decision required (D2):** Confirm that `OwningBuffer<T>` remains the
permanent intrinsic floor for this run, and that the self-hosting goal is
"collections composed over `OwningBuffer<T>`" rather than `OwningBuffer<T>` itself.

---

## 5. Proof-of-Concept Target: `OwningStack<T>`

### 5.1 Why `OwningStack<T>` First

- **Smallest API surface** (8 declared members: `Push`, `Contains`, `Pop`,
  `Peek`, `TryPop`, `TryPeek`, `Clear`, `Drain` — stdlib §4.11; the explicit
  `Contains` was added at v0.222, #1811). No resize-with-insert semantics.
- **Composes trivially** over `OwningArray<T>` (whose reference implementation
  in Bezel is already written at `tests/conformance/cases/stdlib_0039.bzl`).
  In fact, `OwningStack<T>` IS just an `OwningArray<T>` where `Push = Add` and
  `Pop = PopLast`. The self-hosted implementation is approximately 40 lines.
- **No dictionary/hash machinery** (unlike `Set<T>` or `Dictionary<K,V>`).
- **No linked-node pointer chasing** (unlike `LinkedList<T>`).
- **Has an existing working codegen test** at
  `tests/codegen/collection_test.cpp:81–90` (value-element `Stack<int>`), and the
  owned-element variant is exercised in AOT round-trips by
  `owned_collection_pop_dispose_746_test.cpp` and
  `owned_collection_accessors_test.cpp` (the round-trip harness is now the AOT
  path, `runAot`; the former in-process JIT harness was retired). `OwningArray`
  element disposal is thoroughly tested.

### 5.2 Bezel Source for the POC

The self-hosted `OwningStack<T>` composes directly over the `OwningArray<T>`
reference implementation, or equivalently over `OwningBuffer<T>` directly:

```bezel
module System.Collections;

public class OwningStack<T>: IReadOnlyCollection<T> where T: class
  // conformance per v0.222 (#1811): OwningStack was demoted to
  // IReadOnlyCollection<T> only; the IOwningCollection<T> conformance
  // this draft originally declared was removed at that revision.
  private owned _buf: OwningBuffer<T>;
  private _count: int := 0;

  public constructor()
    _buf := new OwningBuffer<T>(8);
  end constructor;

  public method Push(owned item: T)
    if _count = _buf.Length then
      var fresh := new OwningBuffer<T>(_buf.Length * 2);
      for var i in 0..<_count do
        var m := move _buf[i];
        _buf[i] := null;
        fresh[i] := m;
      end;
      _buf.Dispose();
      _buf := fresh;
    end;
    _buf[_count] := item;
    _count := _count + 1;
  end Push;

  public method Pop(): owned T
    if _count = 0 then throw new InvalidOperationException(); end;
    _count := _count - 1;
    var item := move _buf[_count];
    _buf[_count] := null;
    return item!;
  end Pop;

  public method Peek(): T
    if _count = 0 then throw new InvalidOperationException(); end;
    return _buf[_count - 1]!;
  end Peek;

  public method TryPop(out owned item: T): bool
    if _count = 0 then return false; end;
    item := Pop();
    return true;
  end TryPop;

  public method TryPeek(out item: T): bool
    if _count = 0 then return false; end;
    item := _buf[_count - 1]!;
    return true;
  end TryPeek;

  public method Clear()
    for var i in 0..<_count do
      var m := move _buf[i];
      _buf[i] := null;
      m!.Dispose();
    end;
    _count := 0;
  end Clear;

  // Drain: see the IOwningEnumerable pattern (§4.13); deferred to later slice
end OwningStack;
```

[Editorial note, 2026-07-30 refresh: beyond the conformance-header correction
above, this sketch's member set has drifted from the current stdlib §4.11
surface in ways the mechanical refresh did not rewrite — the Try outs are
nullable under the v0.171 Try convention (`TryPop(out owned item: T?): bool`,
`TryPeek(out item: T?): bool`), and v0.222 (#1811) added an explicit
`Contains(item: T): bool`. Bring the POC body up to the v0.231 signatures when
SH-3 is actually scheduled.]

All operations in this source body are already working in the compiler:
- `new OwningBuffer<T>(n)` — lowered via `bz_array_alloc_count_n`
- `_buf[i]` indexer get — lowered by intrinsic OwningBuffer indexer path
- `move _buf[i]` — ownership-checker proven by `generic_owned_field_disposal_test.cpp`
- `_buf[i] := null` — field-restoration rule enforced by ownership checker
- `m!.Dispose()` — vtable dispatch to element's Dispose
- `_buf.Dispose()` — OwningBuffer intrinsic disposal (bz_array_free with disposer)
- `owned item: T` parameter and `return item!` owned-return — proven by
  `generic_owned_field_disposal_test.cpp` (Box pattern at line 156–166)
- `where T: class` constraint — sema registered and codegen monomorphization
  proven by `generic_spec_cache_test.cpp`

### 5.3 What the Compiler Must Do

When `OwningStack<T>` is compiled from the prelude (a self-hosted `.bzl` file):

1. **Sema:** resolve `OwningBuffer<T>` as the intrinsic builtin it already is;
   resolve `move _buf[i]`, `_buf[i] := null`, `item!.Dispose()` via the
   normal expression-typecheck paths for class fields and owned returns.

2. **Codegen:** lower the class through the standard `emitClass` path. The
   `Push` method's `_buf[_count] := item` lowers via the standard
   `lowerCollectionIndexerSet` **because `_buf` is still an OwningBuffer
   intrinsic field** — codegen recognizes OwningBuffer fields and emits
   `bz_array_set_disposing`. The Clear loop's `m!.Dispose()` emits vtable
   dispatch.

3. **Disposal:** the compiler-generated `Dispose` for `OwningStack<T>` sees
   `private owned _buf: OwningBuffer<T>`. `ownedCollectionFieldType` in
   `LlvmBackend_impl.hpp:5498` recognizes OwningBuffer as a collection field
   (via `isCollectionTypeName(rt)` or — since OwningBuffer is in the intrinsic
   set — via the `n == "OwningBuffer"` branch in `LlvmBackend_Layout.cpp:649`).
   The emitted disposal calls `bz_array_free` with the element disposer derived
   from `elementPolicyFor`. This is **unchanged from the current fully-intrinsic
   path** — the self-hosted class's Dispose works identically to today's.

4. **Prelude bootstrap:** after loading the prelude, `TypeSymbol*` for
   `OwningStack` exists in `SemaContext`. Name resolution finds it via the normal
   imported-type path. `builtinTypeArity` returns `kNotBuiltin` for `OwningStack`
   (it has been removed from the list). `isCollectionTypeName` returns `false`
   (because `rt->declaration != nullptr`). Method calls on `OwningStack` go
   through `tryLowerBuiltinClassCall` → standard vtable dispatch path.

The net result: user code `using var s := new OwningStack<MyClass>();` compiles
through the same path as any user-defined generic class, with no special-casing
in the backend for `OwningStack`.

---

## 6. Sequencing and Risk

The work divides into six slices. Each slice is a candidate for one or more PRs
within a dedicated self-hosting run.

### Slice SH-1: Prelude Bootstrap Loader (Prerequisite)

**What:** Add a mechanism to compile a stdlib `.bzl` prelude before user code
and register its `TypeSymbol*` into `SemaContext`. Hook into `Driver.cpp` at the
`compileFile` entry (line 165). Reuse `module::loadDependency` or implement a
dedicated `loadStdlibPrelude(path, semaCtx)` that compiles the prelude to a
`.bezel.meta` object and loads it.

**Scope:** Driver.cpp + a new `src/stdlib/` directory + CMakeLists.txt entry.
No sema or codegen changes yet.

**Test:** A minimal prelude with one class (`class StdHello`); a test that
instantiates `new StdHello()` in user code and verifies the sema does not
report "unresolved type name."

**Size:** Medium (3–5 days). Touches CMake build, driver pipeline, and the
existing module loader. Does not change any collection behavior.

**Risk:** Low for the mechanism; medium for CMake (dependency on build system
order — the prelude must be compiled before any user program using it).

**Owner decision required (D1):** Prelude file location and build-system model.

### Slice SH-2: Expose `OwningBuffer<T>` as Holdable Owned Field for Generic Classes

**What:** This is mostly already done. Verify and test that a user-written generic
class `class Foo<T> where T: class` can declare `private owned _buf: OwningBuffer<T>`
and that:
- Construction (`_buf := new OwningBuffer<T>(n)`) compiles.
- Indexer get (`_buf[i]!`) and owned-move (`move _buf[i]`) compile.
- Indexed set (`_buf[i] := value`) and null-restore (`_buf[i] := null`) compile.
- Class-level Dispose cascades through the OwningBuffer field correctly.

If any of these are untested or broken, fix them before SH-3.

**Tests:** Write an AOT round-trip test (`tests/codegen/owning_buffer_generic_field_test.cpp`)
with a Box-like class that holds `OwningBuffer<T>`, fills some slots, and verifies
the element destructor count at Dispose time.

**Size:** Small-medium (2–3 days). Primarily test writing; may expose 1–2 bugs.

**Risk:** Low. The conformance file `stdlib_0039.bzl` (the `OwningArray<T>` Bezel
source) already exercises all these operations — the risk is in translating from
conformance-check form to AOT round-trip proof.

### Slice SH-3: Self-Host `OwningStack<T>` (Proof of Concept)

**What:** Write `src/stdlib/owning_stack.bzl` containing the `OwningStack<T>`
source above. Wire it into the prelude loader from SH-1. Remove `"OwningStack"`
from `builtinTypeArity` (TypeResolution_Names.cpp line 171) and from the
`isCollectionTypeName` name set (the #1414 stage-3 registry's
`kArrayBackedCollections`, `src/common/BuiltinRegistry.cpp:69` — the predicate
itself, `LlvmBackend_Calls.cpp:397`, reads that registry). Add or
migrate the existing `OwningStack<T>` codegen tests to confirm the self-hosted
path produces the same results as the previous intrinsic path.

**What does NOT change:** `OwningBuffer<T>` intrinsic handling stays. The
`Push`/`Pop`/`Peek` operations on `OwningStack` now reach vtable dispatch instead
of `bz_array_*` calls. The Drain() implementation is deferred.

**Tests:** Migrate the existing OwningStack codegen tests
(`tests/codegen/owned_collection_pop_dispose_746_test.cpp`,
`owned_collection_accessors_test.cpp`; `owned_collection_ops_test.cpp` now covers
only `OwningArray` Remove/Insert); add a new test verifying element disposal via
the self-hosted destructor.

**Size:** Medium (3–5 days). This is the highest-value proof: if it passes, the
architecture is validated.

**Risk:** Medium. The biggest unknown is whether the prelude-loaded TypeSymbol
and its generic instantiation interact correctly with the codegen monomorphization
cache (`generic_spec_cache_test.cpp` patterns). If generic instantiation for an
imported class has bugs, they will surface here. The `#428` decline-with-reasons
memory recorded concerns about cross-module generic construction — this slice
probes exactly that boundary, but with a simpler case (no multi-module separation
of definition site from instantiation site; the prelude is always co-loaded).

**Owner decision required (D3):** Accept that `Drain()` is deferred to a
subsequent slice (it requires `IOwningEnumerable<T>` machinery — stdlib §4.13 —
which is a larger codegen feature).

> **Delegated ruling 2026-08-11 (stdlib-wave readiness; owner review in repo) —
> D3: ACCEPTED.** `Drain()` defers to its own slice. The `IOwningEnumerable<T>`
> protocol machinery (stdlib §4.13) is a separable codegen feature with a fully
> specified stdlib target, so the deferral loses no design information; folding
> it into SH-3 would couple the architecture-validating slice to the largest
> unproven codegen surface and blur the pass/fail signal the slice exists to
> produce. Declined: requiring `Drain()` in SH-3 (couples the proof slice to
> unrelated risk); dropping `Drain()` from the plan (it is required stdlib
> surface — deferral, not removal). Consequence: SH-3's acceptance criteria
> exclude `Drain()`; the follow-up slice picks it up with the §4.13 pattern as
> its spec-shaped test target. (Housekeeping, recorded with the ruling: the
> 2026-07-28 sweep's standing flag 1 — the wholesale pre-v0.168 staleness of
> this document — was discharged by the mechanical refresh performed 2026-07-30,
> per the provenance note above; no refresh gate remains ahead of a 0005 lane.)

### Slice SH-4: Self-Host Borrowing `Stack<T>` and `Queue<T>` / `OwningQueue<T>`

**What:** Write `src/stdlib/stack.bzl`, `src/stdlib/queue.bzl`,
`src/stdlib/owning_queue.bzl`. Borrowing `Stack<T>` and `Queue<T>` are simpler
(no owned-element disposal). Remove their entries from `builtinTypeArity` and
`isCollectionTypeName`.

**Size:** Small-medium per type (1–2 days each).

**Risk:** Low after SH-3 is proven. The borrowing variants use `Array<T>` or a
value `OwningBuffer<int>` as backing, which avoids the owned-element complexity.

### Slice SH-5: Incremental Codegen De-Hardcoding

**What:** For each type migrated off the intrinsic path, audit the ~385 name-
string sites in `src/sema/` and `src/codegen/` and remove or conditionalize the
entries that are now dead. Focus on the high-impact clusters:
- `LlvmBackend_Calls.cpp:397–406` — `isCollectionTypeName` predicate (its name
  set is the #1414 stage-3 registry's `kArrayBackedCollections`,
  `src/common/BuiltinRegistry.cpp:69`).
- `ExprTypecheck_Exprs.cpp:914–919, 1996–2001` — sema member access gates.
- `LlvmBackend_Disposal.cpp:415` + `LlvmBackend_impl.hpp:5498` — owned-field
  disposal classification (moved out of `LlvmBackend_Setup.cpp`).

For each type removed from the predicate, add a regression test that previously-
passing codegen tests still pass.

**Size:** Large (1 week). Primarily surgical removal with test coverage.

**Risk:** Medium. Removing a name from a predicate when the type still has
partial intrinsic handling will cause silent regressions. Gate each removal on a
passing round-trip test.

### Slice SH-6: Remaining Collections (Multi-Run)

This is a multi-run endeavor. Rough ordering by complexity:

| Collection | Complexity | Blocker |
|---|---|---|
| `OwningArray<T>` | Medium | SH-3 proven; reference impl at `stdlib_0039.bzl` |
| `LinkedList<T>` / `OwningLinkedList<T>` | High | Needs node-type definition; `bz_llist_*` is the runtime; self-hosting would need a Bezel-source node class or keep bz_llist_* as the intrinsic floor |
| `Set<T>` / `OwningSet<T>` | High | Hash/eq dispatch is runtime-polymorphic; needs design for how a Bezel-source set class expresses the hash function |
| `Dictionary<K,V>` / `OwningDictionary<K,V>` | Very High | Two type params; generic-key hash table; drain-and-rebuild bucket mutation pattern |

**Owner decision required (D4):** Whether `LinkedList<T>` and `Dictionary<K,V>`
should remain C++ intrinsics long-term (they have the most complex runtime
implementations) or be targeted for Bezel-source rewrite in a later run.

---

## 7. Verdict

**Is it startable?** Yes, but not yet. The keystone prerequisite is the
**prelude-bootstrap loader (Slice SH-1)**: without the ability to compile a
`.bzl` file and inject its `TypeSymbol*` into `SemaContext` before user code
runs, none of the self-hosting replacements can be wired in. That loader does not
exist today.

**The keystone prerequisite is SH-1**, followed immediately by SH-2 (verifying
OwningBuffer<T> as a generic class's owned field works end-to-end in AOT tests).
Once both pass, SH-3 (`OwningStack<T>` as the proof of concept) is the high-
value architectural validation: if a self-hosted generic class with an
`OwningBuffer<T>` field compiles through the prelude path, produces correct AOT
results, and passes element-disposal tests, the architecture is proven and the
remaining collections are mechanical repetition.

**This is a multi-run architectural endeavor.** A single dedicated run should
target SH-1 + SH-2 + SH-3 (prelude bootstrap + OwningBuffer field proof +
`OwningStack<T>` POC). That is four owner-decision gates (D1–D4) and approximately
two weeks of focused work. The remaining collections (SH-4 through SH-6) are
follow-on runs once the POC run validates the architecture. The ~385 hardcoded
name-string sites cannot all be removed in one run without risk of silent
regressions; the correct discipline is to remove each type's entries only after
that type's self-hosted round-trip tests pass.

**First recommended dedicated-run slice:** SH-1 (prelude bootstrap loader) as the
one essential infrastructure piece, immediately followed by SH-2 (OwningBuffer
field AOT proof) and SH-3 (self-hosted `OwningStack<T>`). Commit to these three
and defer SH-4 through SH-6 to subsequent runs. If SH-3 uncovers a fundamental
blocker in cross-module generic instantiation for prelude-loaded types, the run
ends cleanly with a decline-with-reasons issue filed, without having disturbed the
existing intrinsic machinery for the other 11 collection types.
