// Spec-derived regression test for issue #4831 — interface obligations at two
// instantiations that share a generic HEAD (`IM<Box<int>>`, `IM<Box<string>>`)
// were deduplicated by checkAbstractImplementations' BFS, so the second
// obligation was never checked.
//
// §11.1 (v0.307): "A generic interface may be listed at two or more
// instantiations ... §11.2's matching rule is applied once per obligation,
// each under its own instantiation's substitution". §11.2 (v0.307): a member
// matches when its "parameter types ... are exactly equal under the
// interface's substitution".
//
// The BFS in src/sema/TypeResolution_Checks.cpp keyed each visited
// (interface, substitution) pair with `substKey`, which spelled every binding
// by `TypeRef::headName()` only. `T=Box<int>`, `T=Box<string>` and
// `T=Box<int>?` all produced `T=Box;`, so the second instantiation was
// skipped. Measured before the fix (issue repro): `class D: IM<Box<int>>,
// IM<Box<string>>` with only `Get(x: Box<int>)` compiled clean and printed a
// string pointer read as an `int`, plus 1 (type-confused itable dispatch).
//
// REJECT rows assert the §11.2 per-obligation mismatch diagnostic (the
// class declares a `Get`, so the §12.5 "does not implement" name check is
// satisfied and the failure is the §11.2 parameter comparison). ACCEPT rows
// pin that the structural key still merges genuinely identical instantiations
// (a diamond) and still admits a generic class's `IM<Box<T>>`, with exact
// runtime values.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <initializer_list>
#include <string>

using bezel::test::runAot;

namespace {

bool hasErrorContainingAll(const std::string& src,
                           std::initializer_list<const char*> needles) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        bool all = true;
        for (const char* n : needles) {
            if (d.message.find(n) == std::string::npos) { all = false; break; }
        }
        if (all) return true;
    }
    return false;
}

// The issue's repro, verbatim.
const char* kSameHeadTwoInstantiations =
    "module M; "
    "public record Box<T>(V: T); "
    "public interface IM<T> "
    "  method Get(x: T): int; "
    "end IM; "
    "public class D: IM<Box<int>>, IM<Box<string>> "
    "  public constructor() "
    "  end constructor; "
    "  public method Get(x: Box<int>): int => x.V + 1; "
    "end D; "
    "public class Program "
    "  public static method Main() "
    "    using var d := new D(); "
    "    var m: IM<Box<string>> := d; "
    "    Console.WriteLine(m.Get(new Box<string>(\"hello\"))); "
    "  end Main; "
    "end Program;";

// Nullability is part of the instantiation: `IM<Box<int>?>` requires
// `Get(x: Box<int>?)`; `Get(x: Box<int>)` is a narrowing, not §11.2's
// T -> T? widening.
const char* kSameHeadNullableInstantiation =
    "module M; "
    "public record Box<T>(V: T); "
    "public interface IM<T> "
    "  method Get(x: T): int; "
    "end IM; "
    "public class D: IM<Box<int>>, IM<Box<int>?> "
    "  public constructor() "
    "  end constructor; "
    "  public method Get(x: Box<int>): int => x.V + 1; "
    "end D; "
    "public class Program "
    "  public static method Main() "
    "    using var d := new D(); "
    "    var m: IM<Box<int>?> := d; "
    "    Console.WriteLine(m.Get(null)); "
    "  end Main; "
    "end Program;";

// Control: the SAME instantiation reached through two extending interfaces
// is one obligation (a diamond), and is witnessed by the one `Get`.
const char* kDiamondSameInstantiation =
    "module M; "
    "public record Box<T>(V: T); "
    "public interface IM<T> "
    "  method Get(x: T): int; "
    "end IM; "
    "public interface IA: IM<Box<int>> "
    "end IA; "
    "public interface IB: IM<Box<int>> "
    "end IB; "
    "public class D: IA, IB "
    "  public constructor() "
    "  end constructor; "
    "  public method Get(x: Box<int>): int => x.V + 1; "
    "end D; "
    "public class Program "
    "  public static method Main() "
    "    using var d := new D(); "
    "    var m: IM<Box<int>> := d; "
    "    Console.WriteLine(m.Get(new Box<int>(41))); "
    "  end Main; "
    "end Program;";

// Control: a generic class listing `IM<Box<T>>` and implementing
// `Get(x: Box<T>)` matches under its own substitution.
const char* kGenericClassBoxOfT =
    "module M; "
    "public record Box<T>(V: T); "
    "public interface IM<T> "
    "  method Get(x: T): int; "
    "end IM; "
    "public class G<T>: IM<Box<T>> "
    "  public constructor() "
    "  end constructor; "
    "  public method Get(x: Box<T>): int => 7; "
    "end G; "
    "public class Program "
    "  public static method Main() "
    "    using var g := new G<string>(); "
    "    var m: IM<Box<string>> := g; "
    "    Console.WriteLine(m.Get(new Box<string>(\"s\"))); "
    "  end Main; "
    "end Program;";

constexpr const char* kMismatch =
    "method `Get` does not match interface `IM`'s member `Get` on parameter 1";

}  // namespace

BEZEL_TEST(interface_obligation_structural_key_4831,
           same_head_second_instantiation_rejected_11_1) {
    BEZEL_EXPECT_TRUE(
        hasErrorContainingAll(kSameHeadTwoInstantiations, {kMismatch, "§11.2"}));
}

BEZEL_TEST(interface_obligation_structural_key_4831,
           same_head_nullable_instantiation_rejected_11_2) {
    BEZEL_EXPECT_TRUE(hasErrorContainingAll(kSameHeadNullableInstantiation,
                                            {kMismatch, "§11.2"}));
}

BEZEL_TEST(interface_obligation_structural_key_4831,
           diamond_same_instantiation_accepted_runs_11_1) {
    auto r = runAot(kDiamondSameInstantiation);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
}

BEZEL_TEST(interface_obligation_structural_key_4831,
           generic_class_box_of_t_accepted_runs_11_2) {
    auto r = runAot(kGenericClassBoxOfT);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}
