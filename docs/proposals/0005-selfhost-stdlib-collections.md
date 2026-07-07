# Self-Hosting Bezel Standard Library Collections — Design Proposal

**Status:** Draft proposal for owner review. No source changes; this document only.  
**Scope:** Moving the C++ runtime implementations of the stdlib collections into
`.bzl` source that the Bezel compiler compiles, so the compiler's hardcoded
"name == Array" gates progressively atrophy.  
**Context:** Issue #545 direction; rationale §5.11 (v0.97) and §5.20 (v0.132).  
**Provenance:** Drafted mid-June 2026; committed 2026-07-04 as proposal 0005 (it had
sat untracked at the repo root — the custody gap the coverage audit flagged). Written
pre-v0.168 Owning-rename and pre-#1414-stage-3 registry; re-verify name references
against current docs before acting on it.

---

> ⚠️ **PROVENANCE BANNER — mechanical refresh required before any run acts on this.**
> This document's type names (`OwnedStack` / `OwnedBuffer` / `OwnedArray` /
> `OwnedQueue` / `IOwnedCollection`, …) and its `src/` file:line anchors
> **predate the v0.168 Owning-rename and the #1414 stage-3 registry work.** The
> current family is `OwningStack` / `OwningBuffer` / `OwningArray` /
> `OwningQueue` / `IOwningCollection` (spec §14.4, `Bezel_Language_Spec.md:143`
> / `:1927`), and every quoted line number below has almost certainly drifted.
> Before scheduling SH-1/SH-2/SH-3, do a mechanical rename pass and re-grep each
> cited compiler anchor. The architecture and slice plan need no change — only
> the names and line numbers do.

## 1. Current State

### 1.1 C++ Runtime Implementations

Every stdlib collection is implemented in
`src/runtime/bezel_runtime.cpp` (~10,928 lines) as `extern "C"` functions
the LLVM backend calls directly.

**`bz_array_t`** (lines 42–52) backs `Array<T>`, `OwnedArray<T>`, `Stack<T>`,
`OwnedStack<T>`, `Queue<T>`, `OwnedQueue<T>`, `Set<T>`, `OwnedSet<T>`, and
`OwnedBuffer<T>`.  Key functions (lines 54–348):

| Function | Serves |
|---|---|
| `bz_array_alloc` | `new Array<T>()`, `new Stack<T>()`, etc. |
| `bz_array_add` | `xs.Add(x)`, `xs.Push(x)`, `xs.Enqueue(x)` |
| `bz_array_get` / `bz_array_set` | `xs[i]` get/set (value elements) |
| `bz_array_set_disposing` | `xs[i] := v` (class elements, OwnedArray) |
| `bz_array_free` | scope-end `Dispose()`, class elements reversed (§28.11) |
| `bz_array_mark_disposed` | checked-disposal quarantine (§28.2/§621) |
| `bz_array_pop_at` | `OwnedArray.PopAt(i)` — transfer ownership without disposal |
| `bz_array_clear_disposing` | `OwnedArray.Clear()` |
| `bz_array_drain_next_lifo` | Stack drain (LIFO order) |

**`bz_dict_*`** (lines 471–997) backs `Dictionary<K,V>` and
`OwnedDictionary<K,V>`.  Includes separate string-key (`bz_dict_*_str`) and
generic-key (`bz_dict_*_gen`) variants because Dictionary key hashing is
runtime-polymorphic.

**`bz_llist_*`** (lines 1178–1360) backs `LinkedList<T>` and
`OwnedLinkedList<T>`.  Uses a separate doubly-linked node struct.

**`bz_buffer_*`** (lines 10877–10928) backs `Buffer<T>` (value-element fixed-
capacity primitive, issue #545).  Three functions only: `bz_buffer_new`,
`bz_buffer_get_ptr`, `bz_buffer_free`.

**`OwnedBuffer<T>`** with class-typed T is *not* `bz_buffer_*`; it reuses
`bz_array_t` (see `bz_array_alloc_count_n`, line 70; confirmed in
`LlvmBackend_Exprs.cpp` line 5775).

### 1.2 Codegen Lowering — How `new Array<T>()`, `xs.Add(x)`, `xs[i]` are Lowered

**Construction** — `LlvmBackend_Exprs.cpp` lines 5723–5825: a name-switch on
`colName` dispatches each collection type to the appropriate runtime allocator.
`Stack<T>`, `OwnedStack<T>`, `Queue<T>`, `OwnedQueue<T>`, `Set<T>`, `OwnedSet<T>`,
`Array<T>`, `OwnedArray<T>`, `OwnedBuffer<T>` all reach `bz_array_alloc`
(line 5793). `LinkedList<T>` reaches `bz_llist_alloc` (line 5768).
`Dictionary<K,V>` reaches `bz_dict_alloc` or `bz_dict_alloc_generic` (line 5797).

**Method calls** — `LlvmBackend_Calls.cpp` `tryLowerCollectionInstanceCall`
(entry at line 632). The function opens with three predicate calls
(`isCollectionTypeName`, `isLinkedListTypeName`, `isDictTypeName`, lines 639–665),
then dispatches on method name strings (`"Add"`, `"Push"`, `"Enqueue"` at line 2525;
`"Pop"`, `"Dequeue"` at line 2811; etc.).

**Indexer get/set** — `LlvmBackend_Assign.cpp` (`lowerCollectionIndexerSet`) and
`LlvmBackend_Exprs.cpp` (`tryLowerIndexExpr`) dispatch on the same type-name
predicates.

### 1.3 Quantified "Hardcoded Collection Name" Surface

A grep for `"Array"`, `"OwnedArray"`, `"Dictionary"`, `"OwnedDictionary"`,
`"Stack"`, `"OwnedStack"`, `"Queue"`, `"OwnedQueue"`, `"Set"`, `"OwnedSet"`,
`"LinkedList"`, `"OwnedLinkedList"`, `"OwnedBuffer"`, `"Buffer"` across
`src/` (excluding `.h` headers and test/ directories) yields **~180 sites**:

| Subsystem | Approximate count |
|---|---|
| `src/sema/` | ~96 |
| `src/codegen/` | ~79 |
| `src/ownership/` | ~5 |

Representative clusters (the ones a self-host migration would have to displace):

- **`TypeResolution_Names.cpp:55–99`** — `builtinTypeArity`: the master list of
  which names are built-in generics. Stack, Queue, Set, LinkedList, OwnedBuffer
  are listed here; removing them from this list requires they resolve as ordinary
  imported TypeSymbols instead.
- **`LlvmBackend_Calls.cpp:387–411`** — `isCollectionTypeName`,
  `isLinkedListTypeName`, `isDictTypeName` predicates that gate every method-call
  lowering path.
- **`LlvmBackend_Exprs.cpp:5723–5825`** — the `new Coll<T>()` dispatch switch.
- **`ExprTypecheck_Exprs.cpp:291–296, 606–616, 706–711`** — sema checks that
  resolve member types and indexer validity based on literal collection name
  strings.
- **`LlvmBackend_Setup.cpp:2001–2100`** — `emitOwnedFieldDisposals`: the owned-
  field disposal loop that identifies a field as a "collection" (hence requiring
  `bz_array_free` / `bz_llist_free`) by checking `isCollectionTypeName(rt)`.
  **This is the deepest coupling**: a self-hosted collection FIELD in another
  class must still trigger the correct destructor sequence at the field owner's
  Dispose time. If the collection is a user class, the standard vtable-dispatch
  path handles this automatically — this is actually a simplification.

---

## 2. Foundation Already in Place (#545)

### 2.1 Element-Policy Unification

`src/codegen/LlvmBackend_ElementPolicy.cpp` (219 lines) implements
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

`TypeResolution_Names.cpp:99`: `Buffer<T>` registered with arity 1.
`src/runtime/bezel_runtime.cpp` lines 10877–10928: three C functions
(`bz_buffer_new`, `bz_buffer_get_ptr`, `bz_buffer_free`).
`LlvmBackend_Exprs.cpp` line 6026–6065: `new Buffer<T>(n)` lowered to
`bz_buffer_new(len, elem_size)`.

`Buffer<T>` is restricted to primitive value types (int/byte/short/long/double/
decimal/char/bool) per `TypeResolution_Names.cpp:98` comment and
`src/codegen/LlvmBackend_impl.hpp:701`. Class-element `Buffer<T>` is the §5.20
deferred design pass.

### 2.3 `OwnedBuffer<T>` with Class T

Per spec stdlib §4.14 and the `OwnedArray<T>` reference implementation at
`tests/conformance/cases/stdlib_0039.bzl`, `OwnedBuffer<T>` with class T is the
intended storage primitive for every self-hosted owning collection.

The compiler currently lowers `OwnedBuffer<T>` (class T case) as a `bz_array_t*`
with `bz_array_alloc_count_n` (construction), `bz_array_get/set/set_disposing`
(indexer), and `bz_array_free` with the element disposer from `elementPolicyFor`
(disposal). The `move buf[i]` / field-restoration pattern that the reference
implementation uses is exercised by `generic_owned_field_disposal_test.cpp` (the
`Box<T>` test, lines 145–166) and the move-interface-slot tests in
`tests/ownership/`.

**Conclusion:** `OwnedBuffer<T>` for class T is implemented and tested at
the intrinsic level. The stdlib reference implementation of `OwnedArray<T>`
(stdlib §4.14 and `tests/conformance/cases/stdlib_0039.bzl`) is purely
expressed in Bezel source and is the template for self-hosting. The compiler
already understands every operation that source uses.

---

## 3. The Prelude-Bootstrap Gap

### 3.1 How Stdlib Types Are Registered Today

There is **no `.bzl` prelude**. Every stdlib type is registered as a builtin by
name inside `builtinTypeArity` (`TypeResolution_Names.cpp:5–149`). A type not in
that function gets `kNotBuiltin` (line 148) and causes an "unresolved type name"
diagnostic.

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

**Hook point:** `Driver::compileFile` (`src/driver/Driver.cpp` lines 81–204).
The pipeline today is:

```
readFile → Lexer → Parser → collectSymbols → (loadDependency*) → resolveTypes
  → typecheckExpressions → checkDefiniteAssignment → checkOwnership → codegen
```

The prelude loader would run between `SemaContext semaCtx(diagnostics)` and
`collectSymbols(*unit, semaCtx)` — or equivalently, as the first step of
`collectSymbols`, loading the prelude's symbols before the user unit's symbols so
user code can reference them.

**Mechanism:** Reuse the existing cross-module loader infrastructure
(`module::loadDependency` / `ModuleLoader.cpp`). The stdlib prelude is compiled
once to an object + metadata (`bezel.meta`) and loaded as a special "always-
present" dependency. The `SemaContext::importedByModule_` table (lines 171–198
of `SemaContext.hpp`) would map a `"System"` or `"System.Collections"` module
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
as a field** without `OwnedBuffer<T>`. But `OwnedBuffer<T>` is itself a
compiler intrinsic — it has no user `TypeSymbol`; its construction, indexer,
`move buf[i]`, and disposal are all hardcoded in the backend.

The reference implementation of `OwnedArray<T>` (stdlib §4.14) uses
`OwnedBuffer<T>` as its backing field. A self-hosted `OwnedStack<T>` would do the
same: `private owned _storage: OwnedBuffer<T>`. That is **already valid Bezel
source** because:

1. `OwnedBuffer<T>` is a compiler-known type — `builtinTypeArity` returns 1
   for it (line 81 of `TypeResolution_Names.cpp`).
2. Its owned field in a user class triggers `ownedCollectionFieldType` in
   `LlvmBackend_impl.hpp:3213`, which returns the TypeRef so
   `emitOwnedFieldDisposals` (Setup.cpp ~line 2030) calls `bz_array_free` with
   the correct element disposer from `elementPolicyFor`.
3. The `move buf[i]` and `buf[i] := value` indexer operations on an
   `OwnedBuffer<T>` field are lowered by the existing intrinsic paths in
   `LlvmBackend_Exprs.cpp` and `LlvmBackend_Assign.cpp`.

**`OwnedBuffer<T>` as the owned-storage primitive is NOT a gap.** The spec
already designates it as the intrinsic boundary (rationale §5.14, §5.15,
§5.20). After v0.99, "the intrinsic surface of the entire owning-collection
family reduces to one well-scoped primitive (`OwnedBuffer<T>`)" (rationale §5.14).

### 4.2 Options for Any Remaining Gap

The **only residual gap** is that `OwnedBuffer<T>` itself remains a compiler
intrinsic — users cannot rewrite it. The spec explicitly accepts this (stdlib
§4.14: "Why this is a compiler-known primitive"). If the goal is to self-host the
*collections that compose over* `OwnedBuffer<T>`, no additional primitive is
needed.

If in the future even `OwnedBuffer<T>` must be expressible in Bezel source, the
options are:

- **(a) Expose `OwnedBuffer<T>` as a full user TypeSymbol** with a compiler-
  synthesized body for its one intrinsic operation (runtime-sized heap allocation).
  Compiler knows the allocation; everything else (indexer, move, disposal) is
  already standard user-class machinery. Risk: moderate — requires extending the
  prelude bootstrap to handle the one constructor that needs the intrinsic
  allocation call.
- **(b) A new `HeapArray<T>` primitive** at the FFI layer (`extern class`). Risk:
  requires spec amendment (out of scope for implementation PRs per CLAUDE.md).
- **(c) Defer to the §5.20 design pass** — the spec explicitly defers class-
  element `Buffer<T>` and the borrow-storing primitive to future design work.
  Accept `OwnedBuffer<T>` as the permanent intrinsic floor.

**Recommendation:** Option (c). Keep `OwnedBuffer<T>` as the intrinsic floor
and self-host only the collections that compose over it. This matches the spec's
own stated intent and avoids a spec amendment.

**Owner decision required (D2):** Confirm that `OwnedBuffer<T>` remains the
permanent intrinsic floor for this run, and that the self-hosting goal is
"collections composed over `OwnedBuffer<T>`" rather than `OwnedBuffer<T>` itself.

---

## 5. Proof-of-Concept Target: `OwnedStack<T>`

### 5.1 Why `OwnedStack<T>` First

- **Smallest API surface** (6 methods: `Push`, `Pop`, `Peek`, `TryPop`,
  `TryPeek`, `Clear`, `Drain` — stdlib §4.11). No resize-with-insert semantics.
- **Composes trivially** over `OwnedArray<T>` (whose reference implementation
  in Bezel is already written at `tests/conformance/cases/stdlib_0039.bzl`).
  In fact, `OwnedStack<T>` IS just an `OwnedArray<T>` where `Push = Add` and
  `Pop = PopLast`. The self-hosted implementation is approximately 40 lines.
- **No dictionary/hash machinery** (unlike `Set<T>` or `Dictionary<K,V>`).
- **No linked-node pointer chasing** (unlike `LinkedList<T>`).
- **Has an existing working codegen test** at
  `tests/codegen/collection_test.cpp:80–88` (value-element `Stack<int>`); the
  owned-element variant is not yet tested in a JIT round-trip but `OwnedArray`
  element disposal is thoroughly tested.

### 5.2 Bezel Source for the POC

The self-hosted `OwnedStack<T>` composes directly over the `OwnedArray<T>`
reference implementation, or equivalently over `OwnedBuffer<T>` directly:

```bezel
module System.Collections;

public class OwnedStack<T>: IOwnedCollection<T> where T: class
  private owned _buf: OwnedBuffer<T>;
  private _count: int := 0;

  public constructor()
    _buf := new OwnedBuffer<T>(8);
  end constructor;

  public method Push(owned item: T)
    if _count = _buf.Length then
      var fresh := new OwnedBuffer<T>(_buf.Length * 2);
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

  // Drain: see OwnedEnumerable pattern (§4.13); deferred to later slice
end OwnedStack;
```

All operations in this source body are already working in the compiler:
- `new OwnedBuffer<T>(n)` — lowered via `bz_array_alloc_count_n`
- `_buf[i]` indexer get — lowered by intrinsic OwnedBuffer indexer path
- `move _buf[i]` — ownership-checker proven by `generic_owned_field_disposal_test.cpp`
- `_buf[i] := null` — field-restoration rule enforced by ownership checker
- `m!.Dispose()` — vtable dispatch to element's Dispose
- `_buf.Dispose()` — OwnedBuffer intrinsic disposal (bz_array_free with disposer)
- `owned item: T` parameter and `return item!` owned-return — proven by
  `generic_owned_field_disposal_test.cpp` (Box pattern at line 156–166)
- `where T: class` constraint — sema registered and codegen monomorphization
  proven by `generic_spec_cache_test.cpp`

### 5.3 What the Compiler Must Do

When `OwnedStack<T>` is compiled from the prelude (a self-hosted `.bzl` file):

1. **Sema:** resolve `OwnedBuffer<T>` as the intrinsic builtin it already is;
   resolve `move _buf[i]`, `_buf[i] := null`, `item!.Dispose()` via the
   normal expression-typecheck paths for class fields and owned returns.

2. **Codegen:** lower the class through the standard `emitClass` path. The
   `Push` method's `_buf[_count] := item` lowers via the standard
   `lowerCollectionIndexerSet` **because `_buf` is still an OwnedBuffer
   intrinsic field** — codegen recognizes OwnedBuffer fields and emits
   `bz_array_set_disposing`. The Clear loop's `m!.Dispose()` emits vtable
   dispatch.

3. **Disposal:** the compiler-generated `Dispose` for `OwnedStack<T>` sees
   `private owned _buf: OwnedBuffer<T>`. `ownedCollectionFieldType` in
   `LlvmBackend_impl.hpp:3213` recognizes OwnedBuffer as a collection field
   (via `isCollectionTypeName(rt)` or — since OwnedBuffer is in the intrinsic
   set — via the `n == "OwnedBuffer"` branch in `LlvmBackend_Layout.cpp:395`).
   The emitted disposal calls `bz_array_free` with the element disposer derived
   from `elementPolicyFor`. This is **unchanged from the current fully-intrinsic
   path** — the self-hosted class's Dispose works identically to today's.

4. **Prelude bootstrap:** after loading the prelude, `TypeSymbol*` for
   `OwnedStack` exists in `SemaContext`. Name resolution finds it via the normal
   imported-type path. `builtinTypeArity` returns `kNotBuiltin` for `OwnedStack`
   (it has been removed from the list). `isCollectionTypeName` returns `false`
   (because `rt->declaration != nullptr`). Method calls on `OwnedStack` go
   through `tryLowerBuiltinClassCall` → standard vtable dispatch path.

The net result: user code `using var s := new OwnedStack<MyClass>();` compiles
through the same path as any user-defined generic class, with no special-casing
in the backend for `OwnedStack`.

---

## 6. Sequencing and Risk

The work divides into six slices. Each slice is a candidate for one or more PRs
within a dedicated self-hosting run.

### Slice SH-1: Prelude Bootstrap Loader (Prerequisite)

**What:** Add a mechanism to compile a stdlib `.bzl` prelude before user code
and register its `TypeSymbol*` into `SemaContext`. Hook into `Driver.cpp` at the
`compileFile` entry (line 85). Reuse `module::loadDependency` or implement a
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

### Slice SH-2: Expose `OwnedBuffer<T>` as Holdable Owned Field for Generic Classes

**What:** This is mostly already done. Verify and test that a user-written generic
class `class Foo<T> where T: class` can declare `private owned _buf: OwnedBuffer<T>`
and that:
- Construction (`_buf := new OwnedBuffer<T>(n)`) compiles.
- Indexer get (`_buf[i]!`) and owned-move (`move _buf[i]`) compile.
- Indexed set (`_buf[i] := value`) and null-restore (`_buf[i] := null`) compile.
- Class-level Dispose cascades through the OwnedBuffer field correctly.

If any of these are untested or broken, fix them before SH-3.

**Tests:** Write a JIT round-trip test (`tests/codegen/owned_buffer_generic_field_test.cpp`)
with a Box-like class that holds `OwnedBuffer<T>`, fills some slots, and verifies
the element destructor count at Dispose time.

**Size:** Small-medium (2–3 days). Primarily test writing; may expose 1–2 bugs.

**Risk:** Low. The conformance file `stdlib_0039.bzl` (the `OwnedArray<T>` Bezel
source) already exercises all these operations — the risk is in translating from
conformance-check form to JIT round-trip proof.

### Slice SH-3: Self-Host `OwnedStack<T>` (Proof of Concept)

**What:** Write `src/stdlib/owned_stack.bzl` containing the `OwnedStack<T>`
source above. Wire it into the prelude loader from SH-1. Remove `"OwnedStack"`
from `builtinTypeArity` (TypeResolution_Names.cpp line 69) and from the
`isCollectionTypeName` predicate (LlvmBackend_Calls.cpp line 391). Add or
migrate the existing `OwnedStack<T>` codegen tests to confirm the self-hosted
path produces the same results as the previous intrinsic path.

**What does NOT change:** `OwnedBuffer<T>` intrinsic handling stays. The
`Push`/`Pop`/`Peek` operations on `OwnedStack` now reach vtable dispatch instead
of `bz_array_*` calls. The Drain() implementation is deferred.

**Tests:** Migrate `tests/codegen/owned_collection_ops_test.cpp` OwnedStack
sub-cases; add a new test verifying element disposal via the self-hosted destructor.

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
subsequent slice (it requires `OwnedEnumerable<T>` machinery — stdlib §4.13 —
which is a larger codegen feature).

### Slice SH-4: Self-Host Borrowing `Stack<T>` and `Queue<T>` / `OwnedQueue<T>`

**What:** Write `src/stdlib/stack.bzl`, `src/stdlib/queue.bzl`,
`src/stdlib/owned_queue.bzl`. Borrowing `Stack<T>` and `Queue<T>` are simpler
(no owned-element disposal). Remove their entries from `builtinTypeArity` and
`isCollectionTypeName`.

**Size:** Small-medium per type (1–2 days each).

**Risk:** Low after SH-3 is proven. The borrowing variants use `Array<T>` or a
value `OwnedBuffer<int>` as backing, which avoids the owned-element complexity.

### Slice SH-5: Incremental Codegen De-Hardcoding

**What:** For each type migrated off the intrinsic path, audit the 180 name-
string sites in `src/sema/` and `src/codegen/` and remove or conditionalize the
entries that are now dead. Focus on the high-impact clusters:
- `LlvmBackend_Calls.cpp:387–393` — `isCollectionTypeName` predicate.
- `ExprTypecheck_Exprs.cpp:291–296, 706–711` — sema member access gates.
- `LlvmBackend_Setup.cpp:2020–2044` — owned-field disposal classification.

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
| `OwnedArray<T>` | Medium | SH-3 proven; reference impl at `stdlib_0039.bzl` |
| `LinkedList<T>` / `OwnedLinkedList<T>` | High | Needs node-type definition; `bz_llist_*` is the runtime; self-hosting would need a Bezel-source node class or keep bz_llist_* as the intrinsic floor |
| `Set<T>` / `OwnedSet<T>` | High | Hash/eq dispatch is runtime-polymorphic; needs design for how a Bezel-source set class expresses the hash function |
| `Dictionary<K,V>` / `OwnedDictionary<K,V>` | Very High | Two type params; generic-key hash table; drain-and-rebuild bucket mutation pattern |

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
OwnedBuffer<T> as a generic class's owned field works end-to-end in JIT tests).
Once both pass, SH-3 (`OwnedStack<T>` as the proof of concept) is the high-
value architectural validation: if a self-hosted generic class with an
`OwnedBuffer<T>` field compiles through the prelude path, produces correct JIT
results, and passes element-disposal tests, the architecture is proven and the
remaining collections are mechanical repetition.

**This is a multi-run architectural endeavor.** A single dedicated run should
target SH-1 + SH-2 + SH-3 (prelude bootstrap + OwnedBuffer field proof +
`OwnedStack<T>` POC). That is four owner-decision gates (D1–D4) and approximately
two weeks of focused work. The remaining collections (SH-4 through SH-6) are
follow-on runs once the POC run validates the architecture. The 180 hardcoded
name-string sites cannot all be removed in one run without risk of silent
regressions; the correct discipline is to remove each type's entries only after
that type's self-hosted round-trip tests pass.

**First recommended dedicated-run slice:** SH-1 (prelude bootstrap loader) as the
one essential infrastructure piece, immediately followed by SH-2 (OwnedBuffer
field JIT proof) and SH-3 (self-hosted `OwnedStack<T>`). Commit to these three
and defer SH-4 through SH-6 to subsequent runs. If SH-3 uncovers a fundamental
blocker in cross-module generic instantiation for prelude-loaded types, the run
ends cleanly with a decline-with-reasons issue filed, without having disturbed the
existing intrinsic machinery for the other 11 collection types.
