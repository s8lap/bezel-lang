// Regression tests for the surviving half of issue 3562
// (§22.8 step 0 / §22.10 / §12.8, v0.306).
//
// #3583 closed the `new`-site face of this issue's blind index-pairing and left
// the defensive half open: `Emitter::prePopulateBasePositionalFields` still
// paired `baseArgVals[i]` with `basePosFields[i]` by index with no check that
// the base call's argument list describes the base's PRIMARY parameter list.
// This file is the program that reaches that mispairing under the rejections
// #3583 and c3c07e3 put in place, plus the shapes a guard at that store must
// NOT break.
//
// WHAT THE POSITIONAL SLOTS MEAN. §22.8's construction sequence gives the
// base's positional fields exactly one writer:
//
//   "0. Primary-constructor parameters (§22.10) that are referenced in body
//       positions … are captured into their compiler-generated private fields.
//       This step is omitted for classes without a primary constructor."
//
// and, for a convenience constructor,
//
//   "A constructor that chains via `this(...)` **skips steps 0 and 2**: its
//    effective sequence is the chained `this(...)` call, then its own body.
//    (Step 0 is vacuous for such a constructor anyway … primary-parameter
//    capture happens inside the chained-to primary …)"
//
// So the values that land in the base's positional slots are the ones the
// base's PRIMARY constructor was passed. A §22.10 convenience constructor has
// a DIFFERENT parameter list — §22.10 only requires that it chain via
// `this(...)` — so when `base(...)` binds to one, argument `i` does not name
// positional field `i` at all, and pairing them by index is a type-confused
// store into whatever slot happens to sit at that position.
//
// MEASURED ON 7f29b8ce (pre-fix) → this tree. For
//
//   class B(x: int, y: int)
//     public constructor(a: int, d: decimal) this(a, 42); end constructor;
//   class D : B
//     public constructor() base(1, 7.5m); end constructor;
//
// the pre-fix emitter lowered `base(1, 7.5m)` into
//
//   %3 = getelementptr inbounds nuw %class.B, ptr %1, i32 0, i32 3   ; y: int
//   store %bz.decimal { i32 75, i32 0, i32 0, i32 65536 }, ptr %3
//
// — a 16-byte store into the 4-byte `y` slot at offset 16 of a 24-byte object.
// `bezelc --sanitize=address` reported
//
//   ERROR: AddressSanitizer: heap-buffer-overflow …
//   WRITE of size 16 at … 7 bytes after 24-byte region
//
// and the program died on SIGBUS/SIGSEGV (exit 141). With the fix the store is
// gone entirely: `new D()` emits the vtable store and the ctor call, and the
// chained-to primary's step 0 writes both slots.
//
// WHY THE DECIMAL SHAPE IS THE ORACLE. The same defect with a `string` in that
// position emits `store ptr …` into the 4-byte slot — an 8-byte store at a
// 4-byte-aligned offset. That is equally out of contract, but ASan's 8-byte
// fast path reads the shadow granule the store STARTS in (which is valid
// memory) and misses it, so it corrupts the object's trailing padding
// silently. The 16-byte `decimal` store crosses a whole granule and is caught.
// The `string` twin is kept below as a VALUE row so a future change that makes
// it observable has a home.
//
// WHY THE GUARD ROWS ARE HERE. A previous attempt at this issue (`narrow
// #3562`, branch r8-lane-J, held out of the 2026-08-29 gate) guarded the same
// store with `!hasAnyConstructor(t)` and traded the crash for a silently wrong
// object: a `new`/`base(...)` that binds the PRIMARY of a class which merely
// ALSO declares a convenience constructor answered `0` / empty. Every row here
// therefore asserts printed VALUES, not exit status — the loud→quiet trade is
// exactly what an exit-status oracle cannot see.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

// ---------------------------------------------------------------------------
// THE PIN. `base(1, 7.5m)` binds B's §22.10 convenience constructor
// `(a: int, d: decimal)`, whose parameter list is NOT the primary's
// `(x: int, y: int)`. Pre-fix the pre-population stored the 16-byte decimal
// into the 4-byte `y` slot and ran 8 bytes past the allocation.
//
// `runAotChecked` compiles the GENERATED code with AddressSanitizer and flips
// `ran` to false on any sanitizer report, so this row fails on the pre-fix
// compiler at `r.ran` with the heap-buffer-overflow attached. The value
// assertion is what keeps a zeroing "fix" from passing it.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_convenience_ctor_blind_pairing_3562,
           decimal_chain_arg_does_not_overflow_int_slot) {
    auto r = runAotChecked(
        "module M; "
        "class B(x: int, y: int) "
        "  public constructor(a: int, d: decimal) "
        "    this(a, 42); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(x.ToString()); "
        "    Console.WriteLine(y.ToString()); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(1, 7.5m); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n42\n"));
}

// The `string` twin of the same store (`store ptr` into a 4-byte `int` slot).
// It corrupts padding rather than a live byte and ASan's fast path misses it,
// so this row pins the VALUES only — the chained-to primary must still be the
// one that writes both slots.
BEZEL_TEST(base_chain_convenience_ctor_blind_pairing_3562,
           string_chain_arg_into_int_slot_values) {
    auto r = runAot(
        "module M; "
        "class B(x: int, y: int) "
        "  public constructor(a: int, s: string) "
        "    this(a, 42); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(x.ToString()); "
        "    Console.WriteLine(y.ToString()); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(1, \"hello\"); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n42\n"));
}

// ---------------------------------------------------------------------------
// THE GUARD ROWS. Each is a shape whose base positional slots the
// pre-population is still OWED, or which a too-broad guard at that store
// zeroes. They pass identically before and after the fix — that is the point.
// ---------------------------------------------------------------------------

// The refutation of `narrow #3562`'s `!hasAnyConstructor(t)` predicate: the
// `base(...)` call binds B's PRIMARY (arity 2), and B merely ALSO declares an
// arity-1 convenience constructor. `ctorForArgs` answers null here, so
// `ctorChainsViaThis` answers false and the pre-population still runs. Under
// the held commit's guard this printed `0` and an empty line.
BEZEL_TEST(base_chain_convenience_ctor_blind_pairing_3562,
           base_call_binding_the_primary_still_prepopulates) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(v: int) "
        "    this(v, \"conv\"); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(5, \"prim\"); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\nprim\n"));
}

// The #3523 shape: the base has NO explicit constructor at all, so nothing
// downstream writes its positional slots and the pre-population is the only
// writer. `ctorForArgs` answers null (a primary constructor carries no
// `ConstructorSymbol`), the guard stays out of the way, and the slot keeps the
// base call's value rather than its calloc zero.
BEZEL_TEST(base_chain_convenience_ctor_blind_pairing_3562,
           base_without_explicit_ctor_still_prepopulates) {
    auto r = runAot(
        "module M; "
        "class B(n: int) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(7); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// A REORDERED convenience signature reached through `base(...)`: the chain
// restores the primary's order via named arguments. The pre-fix pairing put
// the string in `n: int` and the int in `s: string` and was type-sound only by
// accident of the chained-to primary overwriting both afterwards; the fix must
// keep the values, not zero them (the #3583 `reordered_convenience_named_chain`
// row's `base(...)` twin).
BEZEL_TEST(base_chain_convenience_ctor_blind_pairing_3562,
           reordered_convenience_via_base_call) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(w: string, v: int) "
        "    this(s := w, n := v); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(\"hi\", 3); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nhi\n"));
}

// ---------------------------------------------------------------------------
// THE SECOND PIN: the §22.5 DEFAULT of an omitted slot belongs to step 0 too.
//
// Suppressing only the ARGUMENT stores leaves `numToStore == 0`, and the
// trailing #3777 default-fill loop is unconditional — so it reaches slots it
// had never touched before and evaluates a non-constant default a SECOND time,
// keeping the second value. §22.5 admits exactly these defaults ("call static
// methods, including factory methods that return owned instances") and §22.8
// step 1 is ONE evaluation of the base call, so both the value and the
// side-effect count are wrong.
//
// The `convenience_short_chain_default_via_base_call` row below CANNOT see
// this: its default is the string literal `"dflt"`, an immortal idempotent
// sentinel, so it reads the same however many times it is evaluated. These two
// rows use a counted default instead — the shape
// `base_clause_new_argument_3742.side_effecting_arg_evaluated_once_with_explicit_base_ctor_22_8`
// uses for the same question on the inheritance-clause side.
//
// Measured on `ab43603f` (the intermediate state that suppressed only the
// argument stores) → this tree, against `7f29b8ce` as the baseline:
//
//   k: int := Counter.Next()        7f29b8ce n=3 k=1 cnt=1  → ab43603f k=2 cnt=2
//   owned r: Res := Fac.Make()      7f29b8ce tag=1 made=1   → ab43603f tag=2 made=2
//                                            LSan clean              16 bytes leaked
// ---------------------------------------------------------------------------

// per §22.5 "Optional parameters and named arguments" / §22.8 step 0-1, #3562
BEZEL_TEST(base_chain_convenience_ctor_blind_pairing_3562,
           omitted_default_evaluated_once_via_base_call) {
    auto r = runAot(
        "module M; "
        "class Counter "
        "  public static N: int := 0; "
        "  public static method Next(): int "
        "    N := N + 1; "
        "    return N; "
        "  end Next; "
        "end Counter; "
        "class B(n: int, k: int := Counter.Next()) "
        "  public constructor(w: int, v: int) "
        "    this(v); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine($\"n={n} k={k} cnt={Counter.N}\"); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(9, 3); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // `k=1` is the value §22.8 step 1's single evaluation produces; `cnt=1` is
    // the side-effect count. Both are asserted because either alone can be
    // satisfied by the wrong lowering: a second evaluation that happened to
    // return the same value would keep `k`, and a discarded first evaluation
    // would keep `cnt`.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=3 k=1 cnt=1\n"));
    BEZEL_EXPECT_TRUE(r.stdoutText.find("cnt=2") == std::string::npos);
}

// The `owned` form of the same default. §22.5 names a factory returning an
// owned instance as an admitted default, and a second evaluation overwrites
// the first object's slot before any destructor can reach it — a leak, not
// merely a wrong value. Loop-amplified to 200 constructions so the LSan report
// cannot be dismissed as one allocation still reachable from a live register
// (the one-shot-leak vacuity); pre-repair this leaks 200 objects.
// per §22.5 / §22.8 step 0-1 / §28.2, #3562
BEZEL_TEST(base_chain_convenience_ctor_blind_pairing_3562,
           omitted_owned_default_not_leaked_via_base_call) {
    auto r = runAotChecked(
        "module M; "
        "class Res "
        "  public Tag: int := 0; "
        "  public static Made: int := 0; "
        "  public static Dropped: int := 0; "
        "  destructor "
        "    Dropped := Dropped + 1; "
        "  end destructor; "
        "end Res; "
        "class Fac "
        "  public static method Make(): owned Res "
        "    Res.Made := Res.Made + 1; "
        "    var r := new Res(); "
        "    r.Tag := Res.Made; "
        "    return r; "
        "  end Make; "
        "end Fac; "
        "class B(n: int, owned r: Res := Fac.Make()) "
        "  public constructor(w: int, v: int) "
        "    this(v); "
        "  end constructor; "
        "  public method Tag(): int => r.Tag; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(9, 3); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    var lastTag := 0; "
        "    for var i in 0..<200 do "
        "      using var d := new D(); "
        "      lastTag := d.Tag(); "
        "    end; "
        "    Console.WriteLine($\"tag={lastTag} made={Res.Made} dropped={Res.Dropped}\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // One `Res` per construction, each disposed: 200 made, 200 dropped, and
    // the last object's tag is the 200th. A second evaluation per construction
    // makes it 400 made with 200 of them never dropped.
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("tag=200 made=200 dropped=200\n"));
}

// The §22.5 default must still reach a slot the chain omits — with the
// pre-population out of the way for this shape, the chained-to primary's own
// step 0 is what has to supply `s := "dflt"` (#3777 / #3583's unified body).
// NOTE: a literal default cannot observe double evaluation; the two rows above
// are what pin that. This row pins that the default still ARRIVES.
BEZEL_TEST(base_chain_convenience_ctor_blind_pairing_3562,
           convenience_short_chain_default_via_base_call) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string := \"dflt\") "
        "  public constructor(w: string, v: int) "
        "    this(v); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(\"hi\", 3); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\ndflt\n"));
}

// Three levels: the middle class B is itself a §22.10 class with a §22.10
// inheritance clause `: A(m)`. Declining to pre-populate B's slots for a
// `base(...)` that binds B's convenience constructor must not cost A its own
// value — the ctor call runs B's whole §22.8 sequence, base clause included.
BEZEL_TEST(base_chain_convenience_ctor_blind_pairing_3562,
           three_level_chain_through_convenience_ctor) {
    auto r = runAot(
        "module M; "
        "class A(p: int) "
        "  public method ShowA() "
        "    Console.WriteLine(p.ToString()); "
        "  end ShowA; "
        "end A; "
        "class B(m: int, s: string) : A(m) "
        "  public constructor(v: int) "
        "    this(v, \"conv\"); "
        "  end constructor; "
        "  public method ShowB() "
        "    Console.WriteLine(m.ToString()); "
        "    Console.WriteLine(s); "
        "  end ShowB; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(4); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.ShowB(); "
        "    d.ShowA(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\nconv\n4\n"));
}
