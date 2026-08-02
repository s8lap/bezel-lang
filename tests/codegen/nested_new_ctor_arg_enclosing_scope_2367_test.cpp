// Regression tests for #2367 — a `new T(args)` expression whose argument
// expression is an implicit-field read (a primary-constructor parameter
// referenced only in an initializer position, §22.10, or a captured/explicit
// field read in a method body) was evaluated against the WRONG object when
// `T` itself has a primary constructor (§11.1 / §22.10) with at least one
// positional field.
//
// Root cause (LlvmBackend_Exprs2.cpp, the `new`-expression lowering, the
// `!ctx_.isImported(t)` / `td->hasPositionalFields` branch, ~line 3873-3946):
// constructing `new T(args)` sets `currentThis_ = obj` (the FRESH, not-yet-
// initialized T instance) so that T's OWN field initializers can resolve T's
// OWN primary-ctor params via implicitFieldAddr. The bug: `currentThis_` was
// already flipped to `obj` BEFORE the loop that lowers T's OWN constructor
// ARGUMENT expressions (`ne.args[pi].value`) — but those expressions belong
// to the ENCLOSING scope (the caller's context), not to T's about-to-exist
// instance (§17.7: "`new T(args)` constructs an instance of T" — the args are
// ordinary expressions of the calling context, evaluated before T exists).
// An arg expression that read an implicit field (an identifier resolving via
// implicitFieldAddr — e.g., a positional-ctor param of the ENCLOSING class
// referenced only in an initializer, or an explicit field read in a method
// body) GEP'd through `obj` (T's pointer, sized/typed for T's own struct
// layout) as if `obj` pointed at the ENCLOSING object — a wrong-object,
// wrong-layout GEP. When T's own allocation is smaller than the enclosing
// object's, this reads (and, once stored, propagates) past the end of T's
// heap block: AddressSanitizer heap-buffer-overflow (issue #2367's exact
// report: a 4-byte READ landing 12 bytes past a 16-byte `calloc`). When T's
// allocation happens to be large enough, no OOB READ occurs but the value
// read is still wrong data at the wrong offset — a SILENT correctness bug
// (see `method_body_...` below, which returns 0 pre-fix instead of 55, with
// no sanitizer report at all).
//
// #2367 was originally filed as a throw-safety bug in the no-explicit-
// constructor field-initializer cleanup path (the exact repro below,
// `owned_field_init_...throw...`) because that is where it was first
// observed. Investigation (this fix) found the corruption is unconditional —
// it fires on ANY `new` of a primary-ctor'd type whose argument expression
// reads an implicit field of the enclosing context, exception or no
// exception, field initializer or ordinary method body (see
// `owned_field_init_nested_new_arg_no_crash_asan` and `method_body_...`
// below, neither of which throws). The throw-safety framing in the original
// report is incidental, not the root cause.
//
// The fix restores `currentThis_` to the enclosing scope's `this`
// (`savedThisForInit`) for the duration of lowering each of T's own
// positional-ctor argument expressions, mirroring the SAME save/restore the
// sibling "explicit ctor call-site args" path a little further down in the
// same function already performs correctly (that path's own comment already
// documents the enclosing-scope-expression principle this fix applies here).
//
// Authority: §17.7 (v0.234) "`new T(args)` constructs an instance of T";
// §22.10 (v0.234) primary-constructor parameter scope + the capture-iff-used
// rule (an initializer-only-referenced parameter is NOT captured as a field
// of its own class, but per §22.8 step 0 the implementation still needs a
// slot to hold it across the initializer sequence — a slot on the ENCLOSING
// class, never valid to read through the new object's pointer); §22.8 step 2
// (field initializers, v0.234); §22.9 (owned fields and exception safety,
// v0.234, for the throwing repros).

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// #2367 minimal repro (no exception, no `owned`, no `try`/`catch`): a
// no-explicit-ctor class `Wrapper(first, second)` whose OWN field
// initializer `First := new Leaf(first)` passes `first` — a Wrapper-only
// primary-ctor param, referenced only in this initializer, hence NOT
// captured as a Wrapper field (§22.10 capture rule) but still stored in a
// construction-time slot per §22.8 step 0 — to `new Leaf(first)`, where Leaf
// ALSO has a primary constructor (so the buggy loop fires while lowering
// Leaf's own positional arg-store). Pre-fix: `first` is read through Leaf's
// freshly calloc'd (16-byte) pointer at Wrapper's field-4 offset (28) —
// AddressSanitizer heap-buffer-overflow (the exact #2367 report shape,
// modulo the unrelated exception). Post-fix: `first` reads correctly through
// Wrapper's own object; w.First.V + w.Second == 7 + 13 == 20.
BEZEL_TEST(nested_new_ctor_arg_2367, owned_field_init_nested_new_arg_no_crash_asan) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Leaf(v: int) "
        "  public property V: int => v; "
        "end Leaf; "
        "class Wrapper(first: int, second: int) "
        "  public owned First: Leaf := new Leaf(first); "
        "  public Second: int := second; "
        "end Wrapper; "
        "class C public static method Main(): int "
        "  using var w := new Wrapper(7, 13); "
        "  return w.First.V + w.Second; "
        "end Main; end C;"), 20);
}

// #2367 exact original repro: the SECOND field initializer (`Second := 10 /
// second`) throws DivideByZeroException. Exercises the §22.9 no-explicit-ctor
// throw-safety cleanup (`noctor.field.cleanup`) on top of the wrong-object
// corruption above — `First`'s nested `new Leaf(first)` must complete
// correctly (no OOB) before `Second`'s throw triggers disposal of `First`
// (reverse declaration order) and reclamation of the partially-constructed
// Danger block. runAotChecked additionally proves dispose-exactly-once (ASan
// would catch a double-free) and leak-free (LSan would catch an undisposed
// `First`), not merely "did not crash".
BEZEL_TEST(nested_new_ctor_arg_2367, owned_field_init_nested_new_arg_throw_disposes_cleanly) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Leaf(v: int) "
        "  public property V: int => v; "
        "end Leaf; "
        "class Danger(first: int, second: int) "
        "  public owned First: Leaf := new Leaf(first); "
        "  public Second: int := 10 / second; "
        "end Danger; "
        "class C public static method Main(): int "
        "  var caught: bool := false; "
        "  try "
        "    using var d := new Danger(1, 0); "
        "    return 0; "
        "  catch DivideByZeroException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;"), 42);
}

// #2367 broader-reach pin: the SAME corrupting code path fires for ANY `new`
// of a primary-ctor'd type whose argument reads an implicit field, not just a
// field initializer of a no-explicit-ctor class. `Holder` HAS an explicit
// (empty) constructor — this is NOT the no-explicit-ctor path at all; the
// nested `new Leaf(_x)` is inside an ordinary INSTANCE METHOD body, reading
// Holder's own explicit field `_x`. Pre-fix this did NOT trip ASan (Holder's
// allocation is large enough that the wrong-object GEP stayed in-bounds) —
// it silently returned 0 instead of 55, a wrong-VALUE corruption with no
// sanitizer signal at all. This test's mechanical value check (55, not just
// "ran cleanly") is the load-bearing assertion for that shape.
BEZEL_TEST(nested_new_ctor_arg_2367, method_body_nested_new_arg_reads_enclosing_field) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Leaf(v: int) "
        "  public property V: int => v; "
        "end Leaf; "
        "class Holder "
        "  private _x: int := 55; "
        "  public constructor() end constructor; "
        "  public method Make(): int "
        "    using var l := new Leaf(_x); "
        "    return l.V; "
        "  end Make; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  return h.Make(); "
        "end Main; end C;"), 55);
}

// #2367 adversarial self-pass (c): inheritance — a base class subobject with
// its OWN owned field, combined with a derived no-explicit-ctor class whose
// primary-ctor param feeds a nested `new` (the same corrupting shape) AND
// whose second field initializer throws. Per §22.9's field-initializer-throw
// rule: disposal must cover the derived owned field (`First`, whose
// initializer already completed) in reverse declaration order, THEN the base
// subobject's Dispose() (which the runtime invokes because the base ctor
// completed) — disposing `_baseRes` too. `Leaf.disposed` is bumped by Leaf's
// destructor; both the base's and the derived's Leaf instances must be
// disposed exactly once each: expect 2. runAotChecked's ASan leg additionally
// proves no double-free/UAF across the two disposal sites.
BEZEL_TEST(nested_new_ctor_arg_2367, inheritance_base_and_derived_owned_fields_disposed_once) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Leaf(v: int) "
        "  public static disposed: int := 0; "
        "  public property V: int => v; "
        "  destructor Leaf.disposed := Leaf.disposed + 1; end destructor; "
        "end Leaf; "
        "class Base "
        "  public owned _baseRes: Leaf := new Leaf(100); "
        "end Base; "
        "class Derived(first: int, second: int): Base "
        "  public owned First: Leaf := new Leaf(first); "
        "  public Second: int := 10 / second; "
        "end Derived; "
        "class C public static method Main(): int "
        "  var caught: bool := false; "
        "  try "
        "    using var d := new Derived(1, 0); "
        "    return -1; "
        "  catch DivideByZeroException => caught := true; end; "
        "  return (if caught then Leaf.disposed else -2); "
        "end Main; end C;"), 2);
}
