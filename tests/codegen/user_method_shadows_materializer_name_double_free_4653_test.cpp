// Spec-derived regression tests for issue #4653 — a DOUBLE FREE in accepted
// code when a user method's NAME collides with a stdlib materializer's.
//
// THE DEFECT. Three codegen recognizers in src/codegen/LlvmBackend_Setup.cpp
// identify the stdlib fresh-collection materializers STRUCTURALLY, by the
// callee's member NAME:
//
//   isFreshBorrowingArrayMaterializerLocal  (#1003 — `var/const t := X.ToArray()`)
//   isFreshOwnerlessArrayMaterializerExpr   (#1004 — `X.ToArray().Length`)
//   isFreshOwnerlessDictMaterializerExpr    (#1013 — `X.ToDictionary(k).Count`)
//
// matching `ToArray` / `Sorted` / `Reversed` / `Split` / `ToDictionary`. The
// premise each one's soundness comment states — "these are stdlib builtins
// with no resolved user MethodSymbol", "excluding … any user type with a
// same-named method" — was never ENFORCED: the only other gate is on the
// RESULT type (a declaration-less builtin `Array` / `Dictionary` head), and a
// user method declared `method ToArray(): Array<int>` has exactly that result
// type. So a user method that merely shares the name was recognized as a
// fresh, ownerless materializer and its result was freed by codegen.
//
// Such a method's return is PLAIN — not `owned` — so per §28.3 K1.5 ("Calls
// lend by default: an instance method's plain (non-`owned`) class-typed return
// is a borrow whose source is the receiver") its result is a BORROW of the
// receiver, and §21.1's runtime-`const` ownership clause admits `const t :=
// b.ToArray();` for precisely that reason ("the binding never takes a disposal
// obligation … and **borrows** — the binding is then a borrow carrying the
// initializer's source-liveness constraints"). The receiver keeps the
// obligation. Codegen's extra `bz_array_free` / `bz_dict_free` therefore freed
// storage the receiver still owned and freed again at its own disposal.
//
// MEASURED PRE-FIX (bezelc alone, no harness — the issue's repro):
//   `const t := b.ToArray();`     -> `free(): invalid pointer`, SIGABRT (134)
//   `b.ToArray().Length`          -> SIGSEGV (139)
//   under `--sanitize=address`    -> SEGV in __asan::Allocator::Deallocate
//                                    <- free <- bz_array_free
//                                    (src/runtime/bezel_runtime_collections.cpp:298)
//                                    <- main
// Renaming the method `ToArray` -> `Grab` made the byte-identical program exit
// cleanly (2) — the signature of a name-keyed check.
//
// THE FIX. A shared `calleeIsUserMethod(call, ctx)` helper: the callee resolves
// to a `sema::MethodSymbol` exactly when it is a USER method (declared here or
// imported — the symbol exists for both, only `MemberSymbol::decl` is null for
// a non-generic imported type; see common::calleeMethodDecl). The stdlib
// materializers are compiler intrinsics with no MethodSymbol at all, so the
// guard is false for them and every existing reclaim is unchanged (the
// `builtin_materializers_still_reclaimed` guard row below pins that). A user
// method that genuinely MINTS — `method ToArray(): owned Array<int>` — keeps
// its reclaim through the `owned`-marker channels: unbound temporaries via
// isUnboundOwnedBorrowingArrayCallExpr / common::calleeReturnsOwned, a plain
// `var` binding via §28.3's owned-local auto-drop (the
// `user_owned_returning_toarray_still_reclaimed` row).
//
// ORACLE. `runAotChecked` compiles + links + runs the program as a subprocess
// with generated-code ASan/LSan. A double free is a fired sanitizer report or
// a signal, both of which leave `r.ran == false` -> the -999 sentinel, never a
// legal exit value; so every row asserts the SPECIFIC corrected value the
// program computes. (`failedAtCompileTimeWithRealLocation` would be vacuous
// here: the pre-fix behaviour is a crash, and a crash's backstop diagnostic
// carries a real location on both sides.) No loop amplification is needed —
// unlike a leak, the free is emitted unconditionally and the abort is
// deterministic on every run.
//
// Spec: §28.3 K1.5 (Atoms — "Calls lend by default"), §21.1 *Runtime `const`
// locals* / Ownership (a borrow initializer takes no disposal obligation),
// §28.9 (Array<T>/Dictionary<K,V> trivial dispose), stdlib §4.4 / §5.
// Spec revision v0.307.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A double-free / use-after-free / leak flips r.ran to false -> -999, distinct
// from every legal exit value asserted below.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// §28.3 K1.5 / §21.1 (#4653): THE ISSUE'S REPRO, verbatim in substance — two
// borrowing user methods named after stdlib materializers, both returning the
// same `owned` field, each bound to a runtime `const`. Pre-fix: two spurious
// `bz_array_free` calls over `_items` plus the real one from `b`'s disposal ->
// `free(): invalid pointer` / SIGABRT. Post-fix: one free, exit 1 + 1 = 2.
BEZEL_TEST(codegen_user_method_shadows_materializer_name_double_free_4653,
           two_borrowing_methods_return_same_owned_field_spec_28_3_k1_5) {
    std::string src =
        "module M; "
        "class Box "
        "  private owned _items: Array<int>; "
        "  public constructor() _items := new Array<int>(); _items.Add(7); end constructor; "
        "  destructor end destructor; "
        "  public method ToArray(): Array<int> return _items; end ToArray; "
        "  public method Sorted(): Array<int> return _items; end Sorted; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  const t := b.ToArray(); "
        "  const s := b.Sorted(); "
        "  return t.Count + s.Count; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 2);
}

// §28.3 K1.5 (#4653): the MINIMAL form — one binding is already enough. This
// isolates isFreshBorrowingArrayMaterializerLocal from the two-binding
// arithmetic of the row above; measured pre-fix it aborts on its own (134).
BEZEL_TEST(codegen_user_method_shadows_materializer_name_double_free_4653,
           single_const_binding_of_borrowing_toarray_spec_21_1) {
    std::string src =
        "module M; "
        "class Box "
        "  private owned _items: Array<int>; "
        "  public constructor() _items := new Array<int>(); _items.Add(7); end constructor; "
        "  destructor end destructor; "
        "  public method ToArray(): Array<int> return _items; end ToArray; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  const t := b.ToArray(); "
        "  return t.Count; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 1);
}

// §28.3 K1.5 (#4653): the UNBOUND-RECEIVER shape, which reaches the SECOND
// recognizer (isFreshOwnerlessArrayMaterializerExpr, via
// freeUnboundOwnedBorrowingArrayTemp's `.Length` borrow-position read) rather
// than the local-binding one. Measured pre-fix: SIGSEGV (139).
BEZEL_TEST(codegen_user_method_shadows_materializer_name_double_free_4653,
           unbound_receiver_length_read_of_borrowing_toarray_spec_28_3) {
    std::string src =
        "module M; "
        "class Box "
        "  private owned _items: Array<int>; "
        "  public constructor() _items := new Array<int>(); _items.Add(7); end constructor; "
        "  destructor end destructor; "
        "  public method ToArray(): Array<int> return _items; end ToArray; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  const n := b.ToArray().Count; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 1);
}

// §28.3 K1.5 (#4653): the `Sorted` / `Reversed` names, the other two members of
// the Array whitelist, on a two-element field so the value distinguishes a
// single from a double reclaim.
BEZEL_TEST(codegen_user_method_shadows_materializer_name_double_free_4653,
           sorted_and_reversed_names_shadowed_spec_28_3) {
    std::string src =
        "module M; "
        "class Box "
        "  private owned _items: Array<int>; "
        "  public constructor() _items := new Array<int>(); _items.Add(7); _items.Add(9); end constructor; "
        "  destructor end destructor; "
        "  public method Sorted(): Array<int> return _items; end Sorted; "
        "  public method Reversed(): Array<int> return _items; end Reversed; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  const s := b.Sorted(); "
        "  const r := b.Reversed(); "
        "  return s.Count + r.Count; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 4);
}

// §28.3 K1.5 (#4653): the `Split` name (#1027's addition to the whitelist). The
// element type is `string`, so the spurious reclaim additionally ran the
// string-element release thunk over borrows the field still owns — the same
// double-free one layer down.
BEZEL_TEST(codegen_user_method_shadows_materializer_name_double_free_4653,
           split_name_shadowed_over_array_of_string_spec_28_3) {
    std::string src =
        "module M; "
        "class Doc "
        "  private owned _parts: Array<string>; "
        "  public constructor() _parts := new Array<string>(); _parts.Add(\"a\"); _parts.Add(\"b\"); end constructor; "
        "  destructor end destructor; "
        "  public method Split(sep: string): Array<string> return _parts; end Split; "
        "end Doc; "
        "class C public static method Main(): int "
        "  using var d := new Doc(); "
        "  const p := d.Split(\",\"); "
        "  return p.Count; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 2);
}

// §28.3 K1.5 (#4653): the DICTIONARY sibling — isFreshOwnerlessDictMaterializer
// Expr, reached by a plain `var d := r.ToDictionary();` (#1028's local-binding
// candidate set). Pre-fix the borrowed field dict was `bz_dict_free`d while
// `r` still owned it.
BEZEL_TEST(codegen_user_method_shadows_materializer_name_double_free_4653,
           todictionary_name_shadowed_bound_local_spec_28_3) {
    std::string src =
        "module M; "
        "class Reg "
        "  private owned _map: Dictionary<int, int>; "
        "  public constructor() _map := new Dictionary<int, int>(); _map.Add(1, 7); end constructor; "
        "  destructor end destructor; "
        "  public method ToDictionary(): Dictionary<int, int> return _map; end ToDictionary; "
        "end Reg; "
        "class C public static method Main(): int "
        "  using var r := new Reg(); "
        "  var d := r.ToDictionary(); "
        "  return d.Count; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 1);
}

// §28.3 K1.5 (#4653): the dictionary UNBOUND-receiver shape
// (freeUnboundOwnedBorrowingArrayTemp's bz_dict_free arm).
BEZEL_TEST(codegen_user_method_shadows_materializer_name_double_free_4653,
           todictionary_name_shadowed_unbound_receiver_spec_28_3) {
    std::string src =
        "module M; "
        "class Reg "
        "  private owned _map: Dictionary<int, int>; "
        "  public constructor() _map := new Dictionary<int, int>(); _map.Add(1, 7); end constructor; "
        "  destructor end destructor; "
        "  public method ToDictionary(): Dictionary<int, int> return _map; end ToDictionary; "
        "end Reg; "
        "class C public static method Main(): int "
        "  using var r := new Reg(); "
        "  const n := r.ToDictionary().Count; "
        "  return n; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 1);
}

// GUARD ROW (#4653, not a fail-without row): the fix must be a NO-OP for the
// genuine stdlib intrinsics. `Sorted()`, `Reversed()`, `ToArray()` bound to
// plain locals and an unbound `Sorted().Length` receiver, amplified over 300
// iterations so a suppressed reclaim shows up as an LSan leak (-999) rather
// than passing vacuously on a single reachable allocation. Green both before
// and after the fix by design — it exists so a future widening of
// calleeIsUserMethod that swallowed the intrinsics would be caught here.
// 301 iterations x (3+3+3+3) = 3612; 3612 mod 251 = 98.
BEZEL_TEST(codegen_user_method_shadows_materializer_name_double_free_4653,
           builtin_materializers_still_reclaimed_spec_28_9) {
    std::string src =
        "module M; "
        "class C public static method Main(): int "
        "  using var seq := new Array<int>(); "
        "  seq.Add(3); seq.Add(1); seq.Add(2); "
        "  var total := 0; "
        "  for var i in 0..300 do "
        "    var a := seq.Sorted(); "
        "    var b := seq.Reversed(); "
        "    var c := seq.ToArray(); "
        "    total := total + a.Length + b.Length + c.Length; "
        "    total := total + seq.Sorted().Length; "
        "  end for; "
        "  return total mod 251; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 98);
}

// GUARD ROW (#4653, not a fail-without row): a user method that genuinely MINTS
// — `method ToArray(): owned Array<int>` — must STILL be reclaimed after the
// fix removed it from the name-keyed set. Its channel is the `owned` marker
// (§28.3's owned-local auto-drop for a plain `var` binding), not the name.
// Amplified over 300 iterations so a lost reclaim is an LSan leak (-999).
// 301 iterations x 1 = 301; 301 mod 251 = 50.
BEZEL_TEST(codegen_user_method_shadows_materializer_name_double_free_4653,
           user_owned_returning_toarray_still_reclaimed_spec_28_3) {
    std::string src =
        "module M; "
        "class Mk "
        "  public method ToArray(): owned Array<int> "
        "    var a := new Array<int>(); a.Add(5); return a; "
        "  end ToArray; "
        "end Mk; "
        "class C public static method Main(): int "
        "  using var m := new Mk(); "
        "  var total := 0; "
        "  for var i in 0..300 do "
        "    var t := m.ToArray(); "
        "    total := total + t.Count; "
        "  end for; "
        "  return total mod 251; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 50);
}

// §28.3 K1.5 / §21.1 (#4653): the FUNCTION-TYPED FIELD shape. This is the row
// that settles the predicate. A field `public Split: (Src) -> Array<string>;`
// carries a `FieldSymbol`, NOT a `MethodSymbol`, so a "no MethodSymbol" gate
// lets it straight back through the name match — measured on exactly such a
// build of this fix, the program below still died with
// `AddressSanitizer: SEGV ... in bz_array_free <- Src$DisposeCore <- main`.
// `common::isBuiltinOwnedMaterializerCall` (#2709/#2749) had already recorded
// this case and chosen the "sema resolved NOTHING" gate for it on the
// sema/ownership channels; this row pins that codegen now agrees. Per §28.3's
// producers table a function-value invocation with a non-`owned` concrete
// return is BORROWED, and §21.1 admits borrows verbatim, so the program is
// legal and must run.
BEZEL_TEST(codegen_user_method_shadows_materializer_name_double_free_4653,
           function_typed_field_named_split_is_not_the_intrinsic_spec_28_3) {
    std::string src =
        "module M; "
        "class Src "
        "  private owned _items: Array<string>; "
        "  public constructor() _items := new Array<string>(); _items.Add(\"a\"); _items.Add(\"b\"); end constructor; "
        "  destructor end destructor; "
        "  public method Items(): Array<string> return _items; end Items; "
        "end Src; "
        "class Cfg "
        "  public Split: (Src) -> Array<string> := lambda(s: Src) => s.Items(); "
        "end Cfg; "
        "class C public static method Main(): int "
        "  using var src := new Src(); "
        "  using var cfg := new Cfg(); "
        "  const p := cfg.Split(src); "
        "  return p.Count; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 2);
}

// §28.3 K1.5 (#4653): the FOURTH recognizer — `isFreshGroupByToArrayLocal`, the
// #4308 OWNING twin. Both halves of the shape it matches are name-keyed
// (`ToArray` here, `GroupBy` inside `isGroupByCall`), so a user class declaring
// both members reached the OWNING disposal registration and freed a borrowed
// field array the receiver still owned: measured SEGV in `bz_array_free` <-
// `G$DisposeCore`. Both call nodes must now be sema-symbol-less.
BEZEL_TEST(codegen_user_method_shadows_materializer_name_double_free_4653,
           user_groupby_then_user_toarray_not_the_owning_twin_spec_28_3) {
    std::string src =
        "module M; "
        "class G "
        "  private owned _items: Array<int>; "
        "  public constructor() _items := new Array<int>(); _items.Add(7); end constructor; "
        "  destructor end destructor; "
        "  public method ToArray(): Array<int> return _items; end ToArray; "
        "end G; "
        "class Src "
        "  private owned _g: G; "
        "  public constructor() _g := new G(); end constructor; "
        "  destructor end destructor; "
        "  public method GroupBy(k: int): G return _g; end GroupBy; "
        "end Src; "
        "class C public static method Main(): int "
        "  using var s := new Src(); "
        "  var t := s.GroupBy(1).ToArray(); "
        "  return t.Count; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 1);
}
