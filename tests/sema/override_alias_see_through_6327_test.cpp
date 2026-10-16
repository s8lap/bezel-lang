// #6327 (§12.1, §11.2, §27.8, v0.338): an alias anywhere in a property or
// indexer override's compared type pair must not make the compare BLIND.
//
// #6316 gave the override/interface property-type compare a spelling walk (a
// class type parameter is a declaration-less Named ref, so the blind
// `typeRefEqual` equated any two of them — `P` with `Q`), but switched back to
// the blind compare the moment an alias-expansion marker appeared anywhere in
// either tree. Under §27.8 an alias is a pure synonym, so an alias must not
// change the verdict: `Id<Q>` IS `Q`, and a plainly wrong override whose pair
// merely MENTIONS an alias was accepted.
//
// Two spellings of the alias are covered, because they take different paths:
//  - USE-SITE: the alias is in the derived class's base-type argument list,
//    `B2<P, Id<Q>>`; the base's declared type is the bare `W`.
//  - BASE-SIDE: the BASE declaration itself spells its type through the alias,
//    `abstract property V: Id<W>`; the chain substitution has to reach through
//    the alias expansion to substitute `W`, which it did not.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

const char* kOverrideMsg = "does not match the base member's property type";

}  // namespace

// ---------------------------------------------------------------------------
// USE-SITE alias: the base's value type is `Id<Q>`, the override says `P`.
// §12.1 requires the value type to match; `Id<Q>` is `Q`, not `P`.
// ---------------------------------------------------------------------------
BEZEL_TEST(override_alias_see_through_6327, use_site_alias_wrong_other_parameter_rejected_spec_12_1_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property V: W;
end B2;
public class E<P, Q>: B2<P, Id<Q>>
  public constructor() end constructor;
  public override property V: P => default(P);
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOverrideMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The alias spelled on BOTH sides: `Id<P>` is `P`, and the base says `Id<Q>` is
// `Q` — so this must still be rejected, not laundered by the alias on the
// derived side.
BEZEL_TEST(override_alias_see_through_6327, use_site_alias_on_both_sides_wrong_parameter_rejected_spec_12_1_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property V: W;
end B2;
public class E<P, Q>: B2<P, Id<Q>>
  public constructor() end constructor;
  public override property V: Id<P> => default(Id<P>);
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOverrideMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// An alias OF AN ALIAS (`Id2<T> := Id<T>`) resolves through two expansions.
BEZEL_TEST(override_alias_see_through_6327, use_site_alias_of_alias_wrong_parameter_rejected_spec_12_1_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
type Id2<T> := Id<T>;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property V: W;
end B2;
public class E<P, Q>: B2<P, Id2<Q>>
  public constructor() end constructor;
  public override property V: P => default(P);
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOverrideMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// An alias whose expansion is a CONSTRUCTOR type (`Array<T>`): `Lst<Q>` is
// `Array<Q>`, and `Lst<P>` is `Array<P>` — still not `Array<Q>`.
BEZEL_TEST(override_alias_see_through_6327, use_site_collection_alias_wrong_parameter_rejected_spec_12_1_27_8) {
    auto r = runAot(R"BZ(
module M;
type Lst<T> := Array<T>;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property V: W;
end B2;
public class E<P, Q>: B2<P, Lst<Q>>
  public constructor() end constructor;
  public override property V: Lst<P> => default(Lst<P>);
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOverrideMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The INDEXER's value type travels the same compare as a plain property's.
BEZEL_TEST(override_alias_see_through_6327, use_site_indexer_value_alias_wrong_parameter_rejected_spec_12_1_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property [i: K]: W;
end B2;
public class E<P, Q>: B2<P, Id<Q>>
  public constructor() end constructor;
  public override property [i: P]: P => default(P);
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOverrideMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// BASE-SIDE alias: the base DECLARATION spells its type through the alias.
// ---------------------------------------------------------------------------
BEZEL_TEST(override_alias_see_through_6327, base_side_alias_wrong_other_parameter_rejected_spec_12_1_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property V: Id<W>;
end B2;
public class E<P, Q>: B2<P, Q>
  public constructor() end constructor;
  public override property V: P => default(P);
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOverrideMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// `Id2<W>` expands TWICE before it reaches the class parameter, so the chain
// substitution must follow the whole chain to read `W` as `Q`.
BEZEL_TEST(override_alias_see_through_6327, base_side_alias_of_alias_wrong_parameter_rejected_spec_12_1_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
type Id2<T> := Id<T>;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property V: Id2<W>;
end B2;
public class E<P, Q>: B2<P, Q>
  public constructor() end constructor;
  public override property V: P => default(P);
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOverrideMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A FUNCTION-typed alias: `Pr<W>` is `(W) -> W`, and `(P) -> P` is not
// `(Q) -> Q`.
BEZEL_TEST(override_alias_see_through_6327, base_side_function_alias_wrong_parameter_rejected_spec_12_1_27_8) {
    auto r = runAot(R"BZ(
module M;
type Pr<T> := (T) -> T;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property V: Pr<W>;
end B2;
public class E<P, Q>: B2<P, Q>
  public constructor() end constructor;
  public override property V: Pr<P> => default(Pr<P>);
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOverrideMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A method's RETURN type is compared by the same spelling rule; an alias in the
// pair must not blind it either. (The method return path is
// `checkOverrideSignatureMatch`'s `overrideSpellingDiffers`.)
BEZEL_TEST(override_alias_see_through_6327, method_return_alias_wrong_other_parameter_rejected_spec_12_1_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract method M(): W;
end B2;
public class E<P, Q>: B2<P, Id<Q>>
  public constructor() end constructor;
  public override method M(): P => default(P);
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "does not match the base member's return type"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// ACCEPT rows. Every one of these compiles clean BEFORE the fix too, so they
// are not fail-without proofs — they are the guard the other direction: a fix
// that turns the blind ACCEPT into a blind REJECT would break these. Each
// reads the overridden member through a base-typed reference and prints it,
// so a row whose body is a constant cannot pass here.
// ---------------------------------------------------------------------------
BEZEL_TEST(override_alias_see_through_6327, accepted_use_site_alias_bare_parameter_runs_spec_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
public abstract class B1<K, W>
  public constructor() end constructor;
  public abstract property V: W;
end B1;
public class E1: B1<int, Id<int>>
  public constructor() end constructor;
  public override property V: int => 41;
end E1;
public class Program
  public static method Main(): int
    var b: B1<int, int> := new E1();
    Console.WriteLine(b.V + 1);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// The same pair spelled with the alias on BOTH sides: `Id<int>` is `int` and
// the base's `Id<int>` is `int` too, so the two match (§27.8).
BEZEL_TEST(override_alias_see_through_6327, accepted_use_site_alias_on_both_sides_runs_spec_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
public abstract class B1<K, W>
  public constructor() end constructor;
  public abstract property V: W;
end B1;
public class E1: B1<int, Id<int>>
  public constructor() end constructor;
  public override property V: Id<int> => 40;
end E1;
public class Program
  public static method Main(): int
    var b: B1<int, int> := new E1();
    Console.WriteLine(b.V + 2);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

BEZEL_TEST(override_alias_see_through_6327, accepted_use_site_alias_of_alias_bare_parameter_runs_spec_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
type Id2<T> := Id<T>;
public abstract class B1<K, W>
  public constructor() end constructor;
  public abstract property V: W;
end B1;
public class E1: B1<int, Id2<int>>
  public constructor() end constructor;
  public override property V: int => 39;
end E1;
public class Program
  public static method Main(): int
    var b: B1<int, int> := new E1();
    Console.WriteLine(b.V + 3);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// BASE-SIDE alias, spelled bare on the override: the base declares
// `V: Id<W>`, which with `W := int` IS `int`.
BEZEL_TEST(override_alias_see_through_6327, accepted_base_side_alias_bare_parameter_runs_spec_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property V: Id<W>;
end B2;
public class E1: B2<int, int>
  public constructor() end constructor;
  public override property V: int => 38;
end E1;
public class Program
  public static method Main(): int
    var b: B2<int, int> := new E1();
    Console.WriteLine(b.V + 4);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

BEZEL_TEST(override_alias_see_through_6327, accepted_base_side_alias_of_alias_bare_parameter_runs_spec_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
type Id2<T> := Id<T>;
public abstract class B3<K, W>
  public constructor() end constructor;
  public abstract property V: Id2<W>;
end B3;
public class E1: B3<int, int>
  public constructor() end constructor;
  public override property V: int => 37;
end E1;
public class Program
  public static method Main(): int
    var b: B3<int, int> := new E1();
    Console.WriteLine(b.V + 5);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// The INDEXER's value type through a base-typed read: the alias is on the base
// side, the override names the synonym bare, and the row reads the result.
BEZEL_TEST(override_alias_see_through_6327, accepted_use_site_alias_indexer_value_runs_spec_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
public abstract class B4<K, W>
  public constructor() end constructor;
  public abstract property [i: K]: W;
end B4;
public class E1: B4<int, Id<int>>
  public constructor() end constructor;
  public override property [i: int]: int => i + 1;
end E1;
public class Program
  public static method Main(): int
    var b: B4<int, int> := new E1();
    Console.WriteLine(b[41]);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// The other direction of the matrix: an alias must NOT make two genuinely
BEZEL_TEST(override_alias_see_through_6327, accepted_two_distinct_alias_parameters_stay_distinct_spec_12_1) {
    // The other direction of the matrix: an alias must NOT make two genuinely
    // DIFFERENT class type parameters compare equal. Both base args are spelled
    // through the same alias, and the override swaps them.
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property [i: K]: W;
end B2;
public class E<P, Q>: B2<Id<P>, Id<Q>>
  public constructor() end constructor;
  public override property [i: Id<Q>]: Id<P> => default(Id<P>);
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOverrideMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// THE ISSUE'S OWN REPRO, verbatim (#6327's `p12-alias-blind-pair.bzl`).
//
// Every other row in this file is a per-alias-kind minimisation; this one is
// the reporter's program UNCHANGED — all five alias declarations, the
// intermediate `D<X, Y>: B2<X, Y>`, and BOTH members (the property `V: P` and
// the indexer `[i: P]: P`). It is pinned because the minimised rows each drop
// one of those ingredients, so none of them IS this program: a fix that
// handled the minimised spelling but not the reporter's would leave this row
// green.
//
// §12.1: the base's value type is `W`, which the chain substitution turns into
// `Id<Q>`, i.e. `Q` (§27.8, an alias is a pure synonym). The derived side says
// `P`. §12.1 requires the value type to match exactly — no covariance — so
// BOTH overrides are wrong, and the issue expects an error at both.
// ---------------------------------------------------------------------------
BEZEL_TEST(override_alias_see_through_6327, issue_repro_verbatim_rejected_spec_12_1_27_8) {
    auto r = runAot(R"BZ(
module M;
type Id<T> := T;
type Nl<T> := T?;
type Lst<T> := Array<T>;
type Pr<T> := (T) -> T;
type Id2<T> := Id<T>;
public abstract class B2<K, W>
  public constructor() end constructor;
  public abstract property V: W;
  public abstract property [i: K]: W;
end B2;
public abstract class D<X, Y>: B2<X, Y>
  public constructor() end constructor;
end D;
public class E<P, Q>: B2<P, Id<Q>>
  public constructor() end constructor;
  public override property V: P => throw new Exception("x");
  public override property [i: P]: P => throw new Exception("x");
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kOverrideMsg));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    // The issue expects an error at BOTH overrides (its `V` and its `indexer`),
    // so assert BOTH needles are present. Matched over the CONCATENATED
    // diagnostic text, not the record list: the AOT harness ingests the whole
    // compiler stderr as ONE diagnostic record (aot_support.hpp reports
    // `cc.diag` in a single `diagnostics.report` call), so a per-record count
    // reads 1 even when the compiler emitted two. Without the fix this program
    // is accepted outright and neither needle appears.
    std::string all;
    for (const auto& d : r.diagnostics.diagnostics()) all += d.message;
    BEZEL_EXPECT_TRUE(all.find("override of `V`") != std::string::npos);
    BEZEL_EXPECT_TRUE(all.find("override of `indexer`") != std::string::npos);
}