// fix #1938 / §19.3 (v0.182): a CONCRETE class/interface type-test pattern —
// `case o of C` (a NamedPattern), the typed-binding `var x: C`, and a record
// destructuring `case o of Rec(...)` — over an `Object` scrutinee that actually
// holds a STRING faulted `emitIsA` at run time (SIGSEGV).
//
// §19.3: "Subtypes match: a Dog value matches an Animal pattern"; a type-test
// pattern lowers to a runtime is-a walk (`emitIsA`), which loads the scrutinee's
// word 0 and dereferences it as a type descriptor. A string boxed in an Object
// is a vtable-less `bz_string*` whose word 0 is `char* data`, not a descriptor —
// walking it faults. A string is not a C, so the pattern must cleanly NOT match
// (take the `_` arm), never crash.
//
// The type-PARAMETER pattern path (`case x of T`, #1852) already guarded this in
// its `emitTypeParamPatternTest` helper (a `bz_obj_is_string` short-circuit
// before `emitIsA`), but the CONCRETE-type-name branches of `patternMatches`
// (src/codegen/LlvmBackend_Exprs2.cpp) did not: the NamedPattern class/interface
// descriptor branch, the `var x: C` BindingPattern descriptor branches, and the
// record-destructuring is-a all called `emitIsA` unguarded. The fix routes them
// through the shared `emitStringGuardedIsA` helper — the concrete-name analogue
// of #1852's guard (guard applied only when the static scrutinee type could hold
// a string, i.e. `Object` or an interface; a no-op for a class/interface-typed
// scrutinee, so class/exception dispatch is untouched).
//
// Fail-without/pass-with: WITHOUT the fix each string-in-Object test SIGSEGVs at
// run time; the AOT harness reports a signalled process as ran==false → the
// `run()` helper returns -999, which != the spec-stated value. WITH the fix the
// program runs to normal termination and returns the no-match value. Observable
// is the process exit code (Main returns int; a two-probe test packs
// match/non-match as match*10 + nonmatch).

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// A concrete class type-test (`case o of Animal`) over an Object argument.
// Returns 2 on a match, 9 otherwise.
const char* kClassCase =
    "module M; "
    "class Animal public method K(): int return 1; end K; end Animal; "
    "class Dog    public method K(): int return 2; end K; end Dog; "
    "class P "
    "  public static method Case(o: Object): int "
    "    return case o of Animal => 2, _ => 9 end; "
    "  end Case; "
    "end P; ";

// The typed-binding sibling (`case o of var x: Animal`).
const char* kClassBind =
    "module M; "
    "class Animal public method K(): int return 1; end K; end Animal; "
    "class Dog    public method K(): int return 2; end K; end Dog; "
    "class P "
    "  public static method Bind(o: Object): int "
    "    return case o of var x: Animal => 2, _ => 9 end; "
    "  end Bind; "
    "end P; ";

// An interface type-test (`case o of IShape`).
const char* kIfaceCase =
    "module M; "
    "interface IShape method Area(): int; end IShape; "
    "class Square: IShape public method Area(): int return 4; end Area; end Square; "
    "class Plain  public method P(): int return 0; end P; end Plain; "
    "class P "
    "  public static method Case(o: Object): int "
    "    return case o of IShape => 2, _ => 9 end; "
    "  end Case; "
    "end P; ";

// A record destructuring type-test (`case o of Pt(a, b)`).
const char* kRecordCase =
    "module M; "
    "record Pt(x: int, y: int); "
    "class P "
    "  public static method Case(o: Object): int "
    "    return case o of Pt(a, b) => a + b, _ => 9 end; "
    "  end Case; "
    "end P; ";
}  // namespace

// -- The bug: string-in-Object must cleanly no-match, not fault -----------------

// §19.3 (#1938): a bare `case o of C` (concrete class) over an Object holding a
// STRING must take the `_` arm (a string is not an Animal) — WITHOUT the guard
// emitIsA faults walking the vtable-less bz_string* → SIGSEGV → -999.
BEZEL_TEST(case_concrete_type_string_1938, class_named_pattern_string_no_fault) {
    BEZEL_EXPECT_EQ(run(std::string(kClassCase) +
        "class Program public static method Main(): int "
        "  return P.Case(\"hi\"); "
        "end Main; end Program;"), 9);
}

// §19.3 (#1938): the typed-binding form `var x: C` shares the same fault — a
// string-in-Object must no-match, not fault.
BEZEL_TEST(case_concrete_type_string_1938, class_typed_binding_string_no_fault) {
    BEZEL_EXPECT_EQ(run(std::string(kClassBind) +
        "class Program public static method Main(): int "
        "  return P.Bind(\"hi\"); "
        "end Main; end Program;"), 9);
}

// §19.3 (#1938): an INTERFACE type-test over a string-in-Object — a string
// implements no interface, so no-match, no fault.
BEZEL_TEST(case_concrete_type_string_1938, interface_pattern_string_no_fault) {
    BEZEL_EXPECT_EQ(run(std::string(kIfaceCase) +
        "class Program public static method Main(): int "
        "  return P.Case(\"hi\"); "
        "end Main; end Program;"), 9);
}

// §19.3 (#1938): a RECORD-DESTRUCTURING type-test over a string-in-Object also
// walks the box descriptor via emitIsA — the same fault, guarded by the same
// helper. A string is not a Pt → no-match (9), no fault.
BEZEL_TEST(case_concrete_type_string_1938, record_destructuring_string_no_fault) {
    BEZEL_EXPECT_EQ(run(std::string(kRecordCase) +
        "class Program public static method Main(): int "
        "  return P.Case(\"hi\"); "
        "end Main; end Program;"), 9);
}

// -- The guard must not break genuine matches (no false negatives) -------------

// §19.3 (#1938): the guard is a no-op for a real object — an Animal in an Object
// still matches `case o of Animal` (2) and a Dog still does not (9). 2*10+9 = 29.
BEZEL_TEST(case_concrete_type_string_1938, class_named_pattern_positive_still_matches) {
    BEZEL_EXPECT_EQ(run(std::string(kClassCase) +
        "class Program public static method Main(): int "
        "  using var a := new Animal(); "
        "  using var d := new Dog(); "
        "  return P.Case(a) * 10 + P.Case(d); "
        "end Main; end Program;"), 29);
}

// §19.3 (#1938): the typed-binding form still matches a real Animal (2) and
// rejects a Dog (9) after the guard. 2*10 + 9 = 29.
BEZEL_TEST(case_concrete_type_string_1938, class_typed_binding_positive_still_matches) {
    BEZEL_EXPECT_EQ(run(std::string(kClassBind) +
        "class Program public static method Main(): int "
        "  using var a := new Animal(); "
        "  using var d := new Dog(); "
        "  return P.Bind(a) * 10 + P.Bind(d); "
        "end Main; end Program;"), 29);
}

// §19.3 (#1938): a Square (which implements IShape) still matches the interface
// pattern (2) and a Plain does not (9). 2*10 + 9 = 29.
BEZEL_TEST(case_concrete_type_string_1938, interface_pattern_positive_still_matches) {
    BEZEL_EXPECT_EQ(run(std::string(kIfaceCase) +
        "class Program public static method Main(): int "
        "  using var s := new Square(); "
        "  using var p := new Plain(); "
        "  return P.Case(s) * 10 + P.Case(p); "
        "end Main; end Program;"), 29);
}

// §19.3 / §9.4 (#1938): a boxed record still destructures — a Pt(3,4) in an
// Object matches `case o of Pt(a, b)` and binds the fields (3+4 = 7).
BEZEL_TEST(case_concrete_type_string_1938, record_destructuring_positive_still_matches) {
    BEZEL_EXPECT_EQ(run(std::string(kRecordCase) +
        "class Program public static method Main(): int "
        "  var o: Object := new Pt(3, 4); "
        "  return P.Case(o); "
        "end Main; end Program;"), 7);
}
