// fix #3502 — a `()?` PRIMARY-CONSTRUCTOR positional field never received the
// present unit (spec §4.7 *The unit type*, §22.10 *Primary constructors*,
// §11.1 *Class declaration* — v0.283).
//
// ── THE RULE ────────────────────────────────────────────────────────────────
//
// §4.7, verbatim (v0.283):
//
//   "Wrapping the unit type with `?` (yielding `()?`) is permitted by the
//    grammar and produces an optional with two values, `null` and `()`."
//
// TWO values. A `()?` slot initialised from a unit-typed expression must
// therefore read as PRESENT, distinguishably from one initialised from `null`.
// §22.10 makes a primary-constructor parameter list a storage-declaring form
// ("A class may declare its primary constructor's parameters in the class
// header"), and its parameters are in scope "Inside the inheritance clause's
// base call (see below)" — which is why the direct `new` spelling and the
// `: Base(args)` spelling belong in one test: §22.10 names them as one place.
//
// ── WHAT WAS MEASURED, ON ef339fa (this file's parent) ──────────────────────
//
//     class B(u: ()?)
//       public method Show() Console.WriteLine((u <> null).ToString()); end Show;
//     end B;
//     class D(): B(C.Nothing())  end D;
//
//       new B(C.Nothing())  ->  False      <-- WRONG, expected True
//       new B(null)         ->  False
//       new D()             ->  False      <-- WRONG, expected True
//
// Compile exited 0 with no diagnostic of any severity. `False` for BOTH §4.7
// values through the same slot means the two are indistinguishable there — a
// SILENT WRONG VALUE, not a build failure, which is why every row below pins
// the value and not merely that the program compiled.
//
// ── WHAT THE BUG WAS ────────────────────────────────────────────────────────
//
// Three loops store a positional-field slot from a lowered argument, and all
// three answered a null `lowerExpr` by giving up:
//
//   * `storePrimaryCtorPositionalArgs` (`if (!v) continue;`) — the direct
//     `new B(...)` spelling. The slot kept its calloc zero, and the `()?` zero
//     IS `null`.
//   * `prePopulateBasePositionalFields`, case (a) — the §22.10 inheritance
//     clause. `baseArgVals.clear(); break;` abandoned the WHOLE
//     pre-population, so every one of the base's positional fields stayed zero.
//   * `prePopulateBasePositionalFields`, case (b) — an explicit ctor's leading
//     `base(...)` chain into a primary-ctor base. Same shape.
//
// A unit-typed expression lowers to no `llvm::Value` at all (`()` lowers to
// LLVM `void`), so "no value" was being read as "nothing to store" rather than
// as §4.7's one value. All three now route through `unitValueForTarget` — the
// single predicate #3421 established for "what does a unit source lower to at
// a value-demanding location" — with the target supplied by
// `recordMemberTypeResolved`, the existing authority for a storage member's
// declared type under the active substitution. No new classification predicate
// was added, which is why T6's GENERIC field works for free.
//
// ── WHY EACH ROW IS HERE ────────────────────────────────────────────────────
//
// Every row pins BOTH §4.7 values wherever the shape admits both: a "fix" that
// unconditionally stored the present wrap would read `True` for `null` too, and
// its own control catches that. T5 pins the PAIRING (the rescued value must
// land in the field the argument names, not shift its neighbours), and T7 pins
// that the argument's side effect runs exactly ONCE — the rescue substitutes an
// operand after `lowerExpr` has already emitted the call, so a second
// evaluation would print the marker twice.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

/* §4.7: "It is the return type of methods declared without an explicit return
   type." `Nothing()` is therefore a producer of the unit value, and the `side`
   line makes its evaluation observable. */
constexpr const char* kNothing =
    "  public static method Nothing() "
    "    Console.WriteLine(\"side\"); "
    "  end Nothing; ";

}  // namespace

// ── T1 — the issue's own direct spelling, with its control. `new B(...)` at a
// `()?` positional field must store §4.7's PRESENT value; `new B(null)` must
// store its `null`. Pre-fix BOTH printed False.
BEZEL_TEST(codegen_unit_optional_positional_field_3502,
           direct_new_stores_present_unit_and_null_distinguishably_spec_4_7) {
    auto r = runAot(
        "module M3502a; "
        "class B(u: ()?) "
        "  public method Show() Console.WriteLine((u <> null).ToString()); "
        "  end Show; "
        "end B; "
        "class C " +
        std::string(kNothing) +
        "  public static method Main(): int "
        "    using var b1 := new B(C.Nothing()); "
        "    b1.Show(); "
        "    using var b2 := new B(null); "
        "    b2.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\nTrue\nFalse\n"));
}

// ── T2 — the `()` LITERAL spelling of the same argument. §4.7 gives the type
// "exactly one value, also written `()`", so the literal and the call are two
// spellings of one value and the field must accept both. The literal has always
// lowered to no `llvm::Value`, so its passing is not implied by T1's.
BEZEL_TEST(codegen_unit_optional_positional_field_3502,
           unit_literal_at_a_positional_field_is_present_spec_4_7) {
    auto r = runAot(
        "module M3502b; "
        "class B(u: ()?) "
        "  public method Show() Console.WriteLine((u <> null).ToString()); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new B(()); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n"));
}

// ── T3 — the §22.10 INHERITANCE-CLAUSE spelling (`class D(): B(C.Nothing())`),
// which is `prePopulateBasePositionalFields` case (a). This is the loop whose
// pre-fix `clear(); break;` abandoned the whole pre-population, so it is a
// genuinely different site from T1's, not a re-run of it.
BEZEL_TEST(codegen_unit_optional_positional_field_3502,
           inheritance_clause_base_call_stores_present_unit_spec_4_7_22_10) {
    auto r = runAot(
        "module M3502c; "
        "class B(u: ()?) "
        "  public method Show() Console.WriteLine((u <> null).ToString()); "
        "  end Show; "
        "end B; "
        "class D(): B(C.Nothing()) "
        "end D; "
        "class E(): B(null) "
        "end E; "
        "class C " +
        std::string(kNothing) +
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    using var e := new E(); "
        "    e.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\nTrue\nFalse\n"));
}

// ── T4 — an EXPLICIT constructor's leading `base(...)` chain into a
// primary-ctor base: `prePopulateBasePositionalFields` case (b), the third
// site. The derived constructor carries a parameter deliberately — case (b) is
// gated on `!cs->params.empty() && !ctorArgVals.empty()`, so a zero-parameter
// derived constructor does not reach this loop at all (an unrelated,
// unit-independent gap, filed as #3523; `class D: B` with `constructor()
// base(7)` into `class B(n: int)` reads 0 on this tree too).
BEZEL_TEST(codegen_unit_optional_positional_field_3502,
           explicit_base_chain_into_primary_ctor_base_stores_present_unit_spec_4_7) {
    auto r = runAot(
        "module M3502d; "
        "class B(u: ()?) "
        "  public method Show() Console.WriteLine((u <> null).ToString()); "
        "  end Show; "
        "end B; "
        "class D: B "
        "  public constructor(k: int) "
        "    base(C.Nothing()); "
        "    Console.WriteLine(\"D-ctor\" + k.ToString()); "
        "  end constructor; "
        "end D; "
        "class C " +
        std::string(kNothing) +
        "  public static method Main(): int "
        "    using var d := new D(3); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\nD-ctor3\nTrue\n"));
}

// ── T5 — the PAIRING row. The rescued operand must land in the slot its own
// argument names and leave its neighbour alone. A rescue that pushed nothing
// (the pre-fix `clear()`) zeroed the `int` too; one that pushed the placeholder
// at the wrong index would corrupt it. Both spellings are exercised so the
// pairing is pinned at T1's site and at T3's.
BEZEL_TEST(codegen_unit_optional_positional_field_3502,
           a_rescued_unit_slot_does_not_disturb_its_neighbour_spec_4_7_22_10) {
    auto r = runAot(
        "module M3502e; "
        "class B(u: ()?, n: int) "
        "  public method Show() "
        "    Console.WriteLine((u <> null).ToString() + \"/\" + n.ToString()); "
        "  end Show; "
        "end B; "
        "class D(): B(C.Nothing(), 9) "
        "end D; "
        "class C " +
        std::string(kNothing) +
        "  public static method Main(): int "
        "    using var b := new B(C.Nothing(), 4); "
        "    b.Show(); "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("side\nTrue/4\nside\nTrue/9\n"));
}

// ── T6 — the GENERIC positional field. §7.10 *Generics and nullability*:
// "`where T: value` restricts T to non-nullable value types — records, enums,
// choices ([§35]), primitives, inline arrays (all forms, including
// `[N owned]…` owning forms), and the unit type." So `B<()>` is a legal
// instantiation and `u: T?` at `T = ()` IS a `()?` field. This row is here
// because the fix resolves the field's type through `recordMemberTypeResolved`
// — the substitution-aware authority — rather than testing the declared
// TypeRef directly; a fix that only looked at the syntactic `()?` would fail
// here.
BEZEL_TEST(codegen_unit_optional_positional_field_3502,
           a_generic_positional_field_monomorphized_to_unit_optional_spec_4_7_7_10) {
    auto r = runAot(
        "module M3502f; "
        "class B<T>(u: T?) where T: value "
        "  public method Show() Console.WriteLine((u <> null).ToString()); "
        "  end Show; "
        "end B; "
        "class C " +
        std::string(kNothing) +
        "  public static method Main(): int "
        "    using var a := new B<()>(C.Nothing()); "
        "    a.Show(); "
        "    using var b := new B<()>(null); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\nTrue\nFalse\n"));
}

// ── T7 — EVALUATION COUNT and ORDER. The rescue runs AFTER `lowerExpr` has
// already emitted the argument's side effects; it substitutes an operand, it
// does not re-emit, drop, or reorder the call that produced it. `side` appears
// exactly once per construction (a re-evaluation would print it twice) and
// before `seven` (the rescued argument is the FIRST of the two, so a rescue
// that deferred it would swap the two markers).
BEZEL_TEST(codegen_unit_optional_positional_field_3502,
           the_rescued_unit_argument_is_evaluated_once_and_in_order_spec_4_7) {
    auto r = runAot(
        "module M3502g; "
        "class B(u: ()?, n: int) "
        "  public method Show() "
        "    Console.WriteLine((u <> null).ToString() + \"/\" + n.ToString()); "
        "  end Show; "
        "end B; "
        "class C " +
        std::string(kNothing) +
        "  public static method Seven(): int "
        "    Console.WriteLine(\"seven\"); "
        "    return 7; "
        "  end Seven; "
        "  public static method Main(): int "
        "    using var b := new B(C.Nothing(), C.Seven()); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\nseven\nTrue/7\n"));
}

// ── T8 — the NON-UNIT control. A nullable-VALUE positional field of an
// ordinary type must be untouched by all of the above: the two §7 values stay
// distinguishable and the `clear(); break;` path is never entered for it. This
// is the row that fails if the rescue is made to fire too widely.
BEZEL_TEST(codegen_unit_optional_positional_field_3502,
           an_int_optional_positional_field_is_unchanged_control) {
    auto r = runAot(
        "module M3502h; "
        "class B(n: int?) "
        "  public method Show() "
        "    Console.WriteLine((if n = null then \"null\" else \"set\")); "
        "  end Show; "
        "end B; "
        "class D(): B(5) "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new B(5); "
        "    a.Show(); "
        "    using var b := new B(null); "
        "    b.Show(); "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("set\nnull\nset\n"));
}
