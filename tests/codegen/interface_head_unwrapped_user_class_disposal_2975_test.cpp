// Spec-derived regression test for issue #2975.
//
// §29.7 (iterator disposal) / §29.3 (the iterator vtable format) / §28.3 (the
// `using var` scope-bound auto-drop) / §28.2 (disposal exactly once) / §8.1
// (every class implicitly implements `IDisposable`), v0.240.
//
// THE RULE. §8.1 makes `class MyE: IEnumerable<int>` legal source, and §28.3
// requires a `using var` bound to it to be "disposed at the end of the enclosing
// block" — exactly once (§28.2). §29.3 fixes the ITERATOR vtable layout
// ([0]=td, [1]=Dispose, [2]=MoveNext, [3]=Current), which is a DIFFERENT layout
// from the class vtable ([0]=td$, [1]=itmap$, [2]=Dispose, [3]=ToString). Which
// of the two a slot holds is therefore load-bearing.
//
// THE DEFECT. Both scope-end registrations — `registerUsingDisposal`'s
// `isSyncIterEnum` arm and `registerIterableSourceDisposal` — PREDICTED the
// §29.2 local-decl coercion's outcome from the static types alone: "a
// user-class (declaration-carrying) init is wrapped [into iterator format]".
// The prediction is false. `wrapUserClassAsIEnumerable` DECLINES on five paths
// — no `GetEnumerator`; an unresolvable `GetEnumerator` return type; no
// concrete enumerator class discoverable; no `MoveNext`; `MoveNext` not yet in
// `methodFns_` — and on a decline it returns the source UNCHANGED, leaving the
// RAW class instance in the slot. Dispatching vtable slot 1 on a class object
// then calls its `itmap$` interface-map GLOBAL as a function.
//
// A class whose `GetEnumerator(): IEnumerator<int>` returns a BUILT-IN
// enumerator (`a.GetEnumerator()` on an `Array<int>`) hits the third decline
// path: there is no user enumerator class anywhere in the program to find. That
// is the shape used throughout below.
//
// MEASURED on 618c0c8 (the parent commit), generated-code AddressSanitizer:
//
//   using var e: IEnumerable<int> := new MyE();
//       → "SEGV on unknown address ... Hint: PC is at a non-executable region.
//          Maybe a wild jump?  #0 itmap$MyE"
//   var e: IEnumerable<int> := new MyE();                    (plain `var`)
//       → the same wild jump, via registerIterableSourceDisposal
//   using var r := new MyE(); var e: IEnumerable<int> := r;  (alias)
//       → the same wild jump — and had it dispatched correctly it would have
//         been a DOUBLE dispose, `r` being the owner
//   using var e: IList<int> := new MyE();
//       → no arm matched at all (`classOfLocal` returns null the moment the
//         declared type carries no user declaration), so the instance was never
//         disposed: "Direct leak of 16 byte(s) in 1 object(s)"
//
// THE FIX records what the coercion ACTUALLY did (`iterFormatWrappedDecls_`)
// instead of predicting it, and routes a slot that holds a raw class instance
// to the class disposal — vtable slot 2 + reclaim, which is the dispatch the
// explicit `e.Dispose()` spelling through the same slot already took (measured
// clean on 618c0c8).
//
// WHY THESE ORACLES. A zero exit code proves nothing about disposal — #2921's
// and #2923's filings were both mis-scoped by exactly that inference. Every
// disposal claim below is a LIVE-OBJECT COUNTER: a static `Live` incremented in
// the constructor and decremented in the destructor, plus a `Dtors` tally,
// loop-amplified so a leak is unambiguous. The counter catches BOTH directions
// that LeakSanitizer cannot see together — `Dtors > N` is an over-release (a
// double dispose), `Live > 0` is an under-release — and it fails on the wild
// jump too, since a crashing program returns no counter at all.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// A user class implementing IEnumerable<T> whose GetEnumerator returns a
// BUILT-IN enumerator — so no user enumerator class exists for the §29.2
// coercion to find, and the wrap declines. Carries the live-object counter.
// `IFACE` is the interface head it declares (the axis #2975 manifestation 1 vs
// manifestation 2 differ on).
std::string seqClass(const char* iface) {
    return std::string(
        "class MyE: ") + iface + " "
        "  public static Live: int := 0; "
        "  public static Dtors: int := 0; "
        "  private owned _a: Array<int>; "
        "  public constructor() "
        "    _a := new Array<int>(1); MyE.Live := MyE.Live + 1; "
        "  end constructor; "
        "  destructor "
        "    MyE.Live := MyE.Live - 1; MyE.Dtors := MyE.Dtors + 1; "
        "  end destructor; "
        "  public method GetEnumerator(): IEnumerator<int> "
        "    return _a.GetEnumerator(); "
        "  end GetEnumerator; "
        "end MyE; ";
}

std::string prog(const std::string& mainBody, const std::string& prelude = "") {
    return "module M; " + prelude +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

// 20 iterations of `body`, then: 90 if any instance is still live, 91 if the
// destructor tally is not exactly 20 (an over- OR under-release), else 0.
std::string amplified(const std::string& body) {
    return "var i := 0; "
           "while i < 20 do do " + body + " end; i := i + 1; end; "
           "if MyE.Live <> 0 then return 90; end; "
           "if MyE.Dtors <> 20 then return 91; end; "
           "return 0;";
}

}  // namespace

// ---------------------------------------------------------------------------
// Manifestation 1 (§28.3 auto-drop + §29.3 vtable format): a `using var` at the
// `IEnumerable<T>` head. On 618c0c8 this wild-jumped into `itmap$MyE`.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           using_var_ienumerable_head_disposed_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog(amplified("using var e: IEnumerable<int> := new MyE();"),
                 seqClass("IEnumerable<int>"))),
        0);
}

// The same program under generated-code ASan/LSan: the wild jump was an ASan
// SEGV, and a fix that stopped disposing instead of disposing correctly would
// leak here.
BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           using_var_ienumerable_head_leak_free_under_asan_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(amplified("using var e: IEnumerable<int> := new MyE();"),
                        seqClass("IEnumerable<int>"))),
        0);
}

// ---------------------------------------------------------------------------
// Manifestation 2 (§28.3): the SAME class at the `IList<T>` head. No arm
// matched on 618c0c8, so the instance leaked (LSan: 16 bytes in 1 object).
// A user class MAY declare `IList<int>` with only GetEnumerator defined — the
// issue measured that it compiles, runs, and disposes through the explicit
// path.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           using_var_ilist_head_disposed_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog(amplified("using var e: IList<int> := new MyE();"),
                 seqClass("IList<int>"))),
        0);
}

BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           using_var_ilist_head_leak_free_under_asan_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(amplified("using var e: IList<int> := new MyE();"),
                        seqClass("IList<int>"))),
        0);
}

// ---------------------------------------------------------------------------
// The PLAIN-`var` twin (registerIterableSourceDisposal, not
// registerUsingDisposal). Found while building #2975's grid; the same wild jump
// into `itmap$MyE` on 618c0c8.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           plain_var_ienumerable_head_no_wild_jump_29_3) {
    BEZEL_EXPECT_EQ(
        run(prog(amplified("var e: IEnumerable<int> := new MyE();"),
                 seqClass("IEnumerable<int>"))),
        0);
}

BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           plain_var_ienumerable_head_leak_free_under_asan_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(amplified("var e: IEnumerable<int> := new MyE();"),
                        seqClass("IEnumerable<int>"))),
        0);
}

// ---------------------------------------------------------------------------
// §28.2 exactly-once, the OVER-release direction: an ALIAS of a NAMED owner.
// `r` owns the instance and disposes it at its own scope end, so the widened
// binding must register NOTHING. On 618c0c8 this wild-jumped as well; a fix
// that merely corrected the dispatch WITHOUT the named-owner carve would turn
// it into a double dispose, which `Dtors <> 20` catches (it would be 40).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           alias_of_named_owner_disposed_once_not_twice_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog(amplified("using var r := new MyE(); "
                           "var e: IEnumerable<int> := r;"),
                 seqClass("IEnumerable<int>"))),
        0);
}

BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           alias_of_named_owner_no_double_free_under_asan_28_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(amplified("using var r := new MyE(); "
                                  "var e: IEnumerable<int> := r;"),
                        seqClass("IEnumerable<int>"))),
        0);
}

// The concrete-typed spelling and the explicit-call spelling were the CONTROLS
// that isolated this defect to the widened slot: both were clean on 618c0c8 and
// must stay so — they are the same object model the fix now routes the widened
// slot into.
BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           concrete_typed_using_var_unchanged_28_3) {
    BEZEL_EXPECT_EQ(
        run(prog(amplified("using var e := new MyE();"),
                 seqClass("IEnumerable<int>"))),
        0);
}

BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           explicit_dispose_through_widened_slot_unchanged_28_5) {
    BEZEL_EXPECT_EQ(
        run(prog(amplified("var e: IEnumerable<int> := new MyE(); "
                           "e.Dispose();"),
                 seqClass("IEnumerable<int>"))),
        0);
}

// The OWNERSHIP axis, the two shapes that decide whether routing to the class
// disposal is safe at all. A CallExpr source that mints (`owned MyE`) must be
// disposed here — it has no other owner; a BORROW source must not be, its owner
// disposes it. `Dtors <> 20` catches both errors: 0 for a missed disposal, 40
// for a double one.
BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           owned_returning_call_source_disposed_once_28_8) {
    BEZEL_EXPECT_EQ(
        run(prog(amplified("using var e: IEnumerable<int> := F.Make();"),
                 seqClass("IEnumerable<int>") +
                 "class F "
                 "  public static method Make(): owned MyE "
                 "    return new MyE(); "
                 "  end Make; "
                 "end F; ")),
        0);
}

// A BORROW of an `owned` FIELD widened to the head inside a method: the field's
// class disposes it at ITS scope end, so this binding registers nothing. The
// `isAutoPropertyStorage`/FieldSymbol half of the named-owner carve.
BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           borrowed_owned_field_source_not_disposed_here_28_10) {
    BEZEL_EXPECT_EQ(
        run(prog(
            "var i := 0; var n := 0; "
            "while i < 20 do "
            "  using var h := new H(); n := n + h.Use(); i := i + 1; "
            "end; "
            "if n <> 20 then return 97; end; "
            "if MyE.Dtors <> 20 then return 98; end; return 0;",
            seqClass("IEnumerable<int>") +
            "class H "
            "  private owned _e: MyE; "
            "  public constructor() _e := new MyE(); end constructor; "
            "  public method Use(): int "
            "    var e: IEnumerable<int> := _e; "
            "    return 1; "
            "  end Use; "
            "end H; ")),
        0);
}

// ---------------------------------------------------------------------------
// The BUILT-IN-vs-USER guard, and the reason it is a separate case: a reserved
// built-in ALSO carries a `declaration` (a `builtinClassSym` TypeSymbol, whose
// `kind` is likewise `TypeDeclKind::Class`) and differs only in having no
// `ast::TypeDecl` behind it. The two TypeRef spellings are not symmetric — a
// built-in in an ANNOTATION resolves declaration-less, the same built-in as a
// CALL's result type resolves to the builtinClassSym — so "declaration-less
// declared type + declaration-carrying initializer" does NOT imply a user
// class.
//
// An earlier draft of this fix omitted the `declaration->decl` test and this
// exact program regressed from green to a crash: it matched the raw-user-class
// arm, took the class disposal, and never reached the `ffiBuffer` arm that
// actually frees a `RawBytes` — dispatching a vtable through a `{ptr, len}`
// struct that has none. Pinned here at the point of the judgement so the
// distinction cannot be lost again. (stdlib §10.3; §15.4.)
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           builtin_handle_at_declaration_less_type_keeps_its_own_release_10_3) {
    BEZEL_EXPECT_EQ(
        run(prog(
            "var i := 0; var t := 0; "
            "while i < 200 do "
            "  using var r: RawBytes := RawBytes.From([10, 20, 30, 40]); "
            "  t := t + r.Length; i := i + 1; "
            "end; "
            "if t <> 800 then return 99; end; return 0;")),
        0);
}

// The same distinction on the disposal path for a built-in with a dedicated
// arm further down `registerUsingDisposal`: a `Stopwatch` is a built-in handle
// whose release is `bz_stopwatch_free`, not a vtable dispatch. It must still
// reach that arm. (stdlib §3; §8.1.)
BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           builtin_stopwatch_still_reaches_its_own_arm_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; "
            "while i < 200 do "
            "  using var s: Stopwatch := new Stopwatch(); "
            "  i := i + 1; "
            "end; return 0;")),
        0);
}

// ---------------------------------------------------------------------------
// GUARDS. The fix must not touch the population whose slot genuinely IS in
// iterator format — that is the whole reason the static-name routing #2969 /
// #2970 declined could not be used here. Each of these was measured clean on
// 618c0c8 and must remain clean.
// ---------------------------------------------------------------------------

// The TWO-CLASS idiom: a separate enumerator class with MoveNext + Current, so
// the §29.2 coercion FINDS an enumerator and DOES build the iterator-format
// wrapper. The slot-1 dispatch is correct for it and is left alone.
BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           two_class_idiom_wrap_still_applies_29_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; "
            "while i < 20 do "
            "  using var e: IEnumerable<int> := new MyRange(); "
            "  i := i + 1; "
            "end; return 0;",
            "class RangeEnum "
            "  private _i: int := 0; "
            "  public method MoveNext(): bool "
            "    _i := _i + 1; return _i <= 3; "
            "  end MoveNext; "
            "  public method Current(): int return _i; end Current; "
            "end RangeEnum; "
            "class MyRange: IEnumerable<int> "
            "  private owned _a: Array<int>; "
            "  public constructor() _a := new Array<int>(1); end constructor; "
            "  public method GetEnumerator(): IEnumerator<int> "
            "    return _a.GetEnumerator(); "
            "  end GetEnumerator; "
            "end MyRange; ")),
        0);
}

// The DECISIVE guard on the recorded fact itself. The wrap SUCCEEDS here and
// the source is an unbound `new`, so `ownsSrc` is true and the wrapper's slot-1
// Dispose is what disposes the SOURCE object. If the recorded fact were ever
// read as "not wrapped", this fix's arm would fire and run the CLASS disposal
// on the WRAPPER FRAME instead — the source would never be disposed at all and
// `Dtors` would read 0, not 20. Exit 0 alone could not tell the two apart;
// the counter can.
BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           wrapped_decl_still_disposes_source_through_wrapper_29_11) {
    BEZEL_EXPECT_EQ(
        run(prog(
            "var i := 0; "
            "while i < 20 do do "
            "  using var e: IEnumerable<int> := new MyRange(); "
            "end; i := i + 1; end; "
            "if MyRange.Dtors <> 20 then return 94; end; return 0;",
            "class RangeEnum "
            "  private _i: int := 0; "
            "  public method MoveNext(): bool "
            "    _i := _i + 1; return _i <= 3; "
            "  end MoveNext; "
            "  public method Current(): int return _i; end Current; "
            "end RangeEnum; "
            "class MyRange: IEnumerable<int> "
            "  public static Dtors: int := 0; "
            "  destructor MyRange.Dtors := MyRange.Dtors + 1; end destructor; "
            "  private owned _a: Array<int>; "
            "  public constructor() _a := new Array<int>(1); end constructor; "
            "  public method GetEnumerator(): IEnumerator<int> "
            "    return _a.GetEnumerator(); "
            "  end GetEnumerator; "
            "end MyRange; ")),
        0);
}

// The SELF-ENUMERATING idiom (#2384): the class implements both halves, so
// `structurallyIsEnumerator(srcTs)` is true and the wrap succeeds. Written as
// #2384's own self-returning shape (`return this;`): the earlier body,
// `using var a := …; return a.GetEnumerator();`, is the dangling-enumerator
// return #4931 rejects (§28.3, §29.11).
BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           self_enumerating_idiom_wrap_still_applies_29_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; "
            "while i < 20 do "
            "  using var e: IEnumerable<int> := new SelfSeq(); "
            "  i := i + 1; "
            "end; return 0;",
            "class SelfSeq: IEnumerable<int>, IEnumerator<int> "
            "  private _i: int := 0; "
            "  public method GetEnumerator(): IEnumerator<int> "
            "    return this; "
            "  end GetEnumerator; "
            "  public method MoveNext(): bool "
            "    _i := _i + 1; return _i <= 3; "
            "  end MoveNext; "
            "  public property Current: int get => _i; end Current; "
            "end SelfSeq; ")),
        0);
}

// §29.11: a YIELD-METHOD iterator bound to the same head. Its value is a real
// iterator frame — the population the #2969/#2970 decline measured would be
// broken by any runtime "has a vtable" gate. It has no user-class static type,
// so the fix never looks at it.
BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           yield_iterator_source_unchanged_29_11) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var tot := 0; var i := 0; "
            "while i < 20 do "
            "  using var e: IEnumerable<int> := P.Seq(); "
            "  for var v in e do tot := tot + v; end; "
            "  i := i + 1; "
            "end; "
            "if tot <> 60 then return 92; end; return 0;",
            "class P "
            "  public static method Seq(): IEnumerable<int> "
            "    yield return 1; yield return 2; "
            "  end Seq; "
            "end P; ")),
        0);
}

// stdlib §5: a LAZY LINQ pipeline bound to the same head — the other population
// the #2969/#2970 decline measured. Its static type is the lazy sentinel, not a
// user class, so the fix never looks at it either.
BEZEL_TEST(codegen_iface_head_raw_user_class_2975,
           lazy_linq_pipeline_source_unchanged_5) {
    BEZEL_EXPECT_EQ(
        runChecked(std::string(
            "module M; import System.Collections.Linq; "
            "class C public static method Main(): int "
            "  var tot := 0; var i := 0; "
            "  while i < 20 do "
            "    using var src := new Array<int>(4); "
            "    src.Add(1); src.Add(2); src.Add(3); "
            "    using var e: IEnumerable<int> := "
            "        src.Where(lambda(x) => x > 1); "
            "    for var v in e do tot := tot + v; end; "
            "    i := i + 1; "
            "  end; "
            "  if tot <> 100 then return 93; end; "
            "  return 0; "
            "end Main; end C;")),
        0);
}
