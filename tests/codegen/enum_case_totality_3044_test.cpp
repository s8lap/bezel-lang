// fix #3044 — the §21.7 statement-`case` totality grant, and §9.8's deleted
// write-side raise (proposal 0078, graduated spec v0.242).
//
// §21.7 (v0.242) admits the **enum** constants-coverage bullet to condition
// (i)'s closed-value-space list: "§5.4 closes the value space to the declared
// values: every producer of an enum value yields a declared value — checked
// casts, §9.8 deserialization, §17.11's zero-constant requirement — so the
// no-match fall-through of a statically exhaustive enum `case` statement is
// unreachable". Through v0.241 §21.7 recorded the opposite, on the ground that
// a non-constant `E(n)` could mint a raw value — repealed with §5.4's rule.
//
// §9.8 (v0.242): the synthesized enum `Serialize` has "no failing case … every
// enum value is a declared value (§5.4)", so its write-side
// `SerializationException` arm is unreachable and is deleted.
//
// The sema verdicts are the lit pair sema_ret_path__{exhaustive,nonexhaustive}_
// enum_case_stmt_*; this file pins that the granted shapes still COMPUTE
// correctly at runtime.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
// 0, 1, 2 by §10.1 auto-increment.
const char* kPriority = "module M; enum Priority Low, Medium, High; end Priority; ";
}  // namespace

// ---------------------------------------------------------------------------
// §21.7 / §19.6 — the totality grant the §5.4 invariant unlocks
// ---------------------------------------------------------------------------

// "§21.7's enum row flips: the constants-coverage proof now grants statement-
// `case` totality" — a method may END with a constants-exhaustive enum `case`
// whose every arm returns, with no trailing `return`, and it must still compute
// the right answer at runtime.
BEZEL_TEST(enum_case_totality_3044, exhaustive_enum_case_stmt_is_total_spec_21_7) {
    BEZEL_EXPECT_EQ(run(std::string(kPriority) +
        "class C "
        "  public static method Rank(p: Priority): int "
        "    case p of "
        "      Low    => return 10;, "
        "      Medium => return 20;, "
        "      High   => return 30; "
        "    end; "
        "  end Rank; "
        "  public static method Main(): int "
        "    var n: int := 1; "
        "    return Rank(Priority(n)) + Rank(Priority.High) - 8; "  // 20+30-8
        "  end Main; "
        "end C;"), 42);
}

// ---------------------------------------------------------------------------
// §9.8 — the write-side raise is deleted; synthesis still round-trips
// ---------------------------------------------------------------------------

// "on write no failing case exists — every enum value is a declared value
// (§5.4)". §9.8's write-side `SerializationException` arm is therefore
// unreachable and is removed; the LAST-declared constant becomes the
// unconditional fallback write. §10.2's shared-value rule is the case that
// rule interacts with: the synthesized write must still pick the
// FIRST-declared name for a duplicated value, and the read must accept every
// declared name. (The first/middle/last single-value round trips are already
// pinned by the `codegen_serialization_550` enum family.)
BEZEL_TEST(enum_serialize_write_3044, duplicate_value_first_declared_name_spec_10_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Serialization; "
        "enum E: ISerializable, IDeserializable Legacy := 1, Current := 1; "
        "end E; "
        "class C public static method Main(): int "
        "  using var s := new BinarySerializer(); "
        "  E.Current.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  var back := E.Deserialize(d); "
        "  return (if back = E.Legacy then 42 else 1); "
        "end Main; end C;"), 42);
}
