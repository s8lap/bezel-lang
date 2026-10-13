// fix #5996 / spec §11.2 (v0.338): a member matches an interface member when
// its parameter types are "exactly equal under the interface's substitution"
// (§11.2 rule 2, "No widening elsewhere", L1845). A built-in class that has no
// user declaration (`Mutex`, `Random`, `StringBuilder`, `Exception`) is NOT a
// member's own type parameter, so two different built-ins never match, at the
// top level or nested inside a generic argument. Only a member's OWN type
// parameters correspond by position, not by name (§11.2 *Generic members*,
// L1849), and a method-level parameter is told apart from a built-in by the
// member's declared generic parameter list, never by spelling shape.
//
// Reject rows assert `!r.ran` and the §11.2 needle. Run rows assert `r.ran`
// and the program's stdout. The mismatching programs used to compile and run
// the WRONG body on a wrong-class object.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasConformanceDiagnostic(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos &&
            d.message.find("(\xC2\xA7""11.2)") != std::string::npos)
            return true;
    return false;
}

}  // namespace

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           mutex_vs_random_top_level_rejected_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface ISink "
        "  method Take(m: Mutex); "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Take(m: Random) "
        "  end Take; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink := new Feeder(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasConformanceDiagnostic(
        r, "method `Take` does not match interface `ISink`'s member `Take`"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           array_of_mutex_vs_array_of_random_nested_rejected_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface ISink "
        "  method Take(xs: Array<Mutex>); "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Take(xs: Array<Random>) "
        "  end Take; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink := new Feeder(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasConformanceDiagnostic(
        r, "method `Take` does not match interface `ISink`'s member `Take`"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           stringbuilder_vs_exception_issue_repro_rejected_spec_11_2) {
    // The issue repro: `Take(e: Exception)` ran with a StringBuilder object.
    auto r = runAot(
        "module M; "
        "public interface ISink "
        "  method Take(s: StringBuilder); "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Take(e: Exception) "
        "    Console.WriteLine(\"Feeder.Take ran\"); "
        "    Console.WriteLine(e.Message); "
        "  end Take; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink := new Feeder(); "
        "    s.Take(new StringBuilder()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasConformanceDiagnostic(
        r, "method `Take` does not match interface `ISink`'s member `Take`"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           same_builtin_class_runs_spec_11_2) {
    // Control: the same built-in on both sides still matches.
    auto r = runAot(
        "module M; "
        "public interface ISink "
        "  method Take(s: StringBuilder); "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Take(b: StringBuilder) "
        "    Console.WriteLine(\"took\"); "
        "  end Take; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink := new Feeder(); "
        "    s.Take(new StringBuilder()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("took\n"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           renamed_method_type_param_top_level_runs_spec_11_2) {
    // §11.2 *Generic members* (L1849): `Show<U>(x: U)` matches `Show<T>(x: T)`.
    auto r = runAot(
        "module M; "
        "public interface ISink "
        "  method Show<T>(x: T): string; "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Show<U>(x: U): string "
        "    return \"shown\"; "
        "  end Show; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink := new Feeder(); "
        "    Console.WriteLine(s.Show<int>(3)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("shown\n"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           renamed_method_type_param_nested_runs_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface ISink "
        "  method Show<T>(xs: Array<T>): int; "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Show<U>(xs: Array<U>): int "
        "    return xs.Count; "
        "  end Show; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink := new Feeder(); "
        "    Console.WriteLine(s.Show<int>([7, 8, 9]).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           method_type_param_named_like_builtin_runs_spec_11_2) {
    // A method-level type parameter that shares a built-in's name denotes the
    // parameter (name lookup resolves the member's own parameter first), and
    // corresponds by position to the interface member's parameter.
    auto r = runAot(
        "module M; "
        "public interface ISink "
        "  method Show<Mutex>(m: Mutex): string; "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Show<Random>(m: Random): string "
        "    return \"shadow\"; "
        "  end Show; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink := new Feeder(); "
        "    Console.WriteLine(s.Show<int>(1)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("shadow\n"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           enclosing_type_param_nested_runs_spec_11_2) {
    // `Gen<T>: ISink<T>` with `Take(xs: Array<T>)`: the enclosing type's own
    // parameter, substituted through the interface's, still matches.
    auto r = runAot(
        "module M; "
        "public interface ISink<V> "
        "  method Take(xs: Array<V>); "
        "end ISink; "
        "public class Gen<T>: ISink<T> "
        "  public constructor() end constructor; "
        "  public method Take(xs: Array<T>) "
        "    Console.WriteLine(xs.Count.ToString()); "
        "  end Take; "
        "end Gen; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink<int> := new Gen<int>(); "
        "    s.Take([1, 2, 3, 4]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           inherited_generic_interface_runs_spec_11_2) {
    // The obligation arrives through an inherited generic interface.
    auto r = runAot(
        "module M; "
        "public interface IBase<V> "
        "  method Take(xs: Array<V>); "
        "end IBase; "
        "public interface IChild<W>: IBase<W> "
        "end IChild; "
        "public class Feeder: IChild<int> "
        "  public constructor() end constructor; "
        "  public method Take(xs: Array<int>) "
        "    Console.WriteLine(xs.Count.ToString()); "
        "  end Take; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: IChild<int> := new Feeder(); "
        "    s.Take([1, 2]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           alias_on_interface_side_runs_spec_27_8) {
    // §27.8 (v0.338): an alias is a pure synonym. `type A := Array<int>` on the interface side.
    auto r = runAot(
        "module M; "
        "type A := Array<int>; "
        "public interface ISink "
        "  method Show(xs: A); "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Show(xs: Array<int>) "
        "    Console.WriteLine(\"ran-\" + xs.Count.ToString()); "
        "  end Show; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink := new Feeder(); "
        "    s.Show([1, 2]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran-2\n"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           alias_on_implementer_side_runs_spec_27_8) {
    // §27.8: alias on the implementer side.
    auto r = runAot(
        "module M; "
        "type A := Array<int>; "
        "public interface ISink "
        "  method Show(xs: Array<int>); "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Show(xs: A) "
        "    Console.WriteLine(\"ran-\" + xs.Count.ToString()); "
        "  end Show; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink := new Feeder(); "
        "    s.Show([1, 2, 3]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran-3\n"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           two_aliases_of_one_type_run_spec_27_8) {
    // §27.8: two different aliases of one type.
    auto r = runAot(
        "module M; "
        "type A := Array<int>; type B := Array<int>; "
        "public interface ISink "
        "  method Show(xs: A); "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Show(xs: B) "
        "    Console.WriteLine(\"ran-\" + xs.Count.ToString()); "
        "  end Show; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink := new Feeder(); "
        "    s.Show([4]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran-1\n"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           alias_of_alias_runs_spec_27_8) {
    // §27.8: alias of an alias.
    auto r = runAot(
        "module M; "
        "type A := Array<int>; type B := A; "
        "public interface ISink "
        "  method Show(xs: B); "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Show(xs: Array<int>) "
        "    Console.WriteLine(\"ran-\" + xs.Count.ToString()); "
        "  end Show; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink := new Feeder(); "
        "    s.Show([4, 5, 6, 7]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran-4\n"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           generic_alias_runs_spec_27_8) {
    // §27.8: generic alias `type L<T> := Array<T>` at `L<int>`.
    auto r = runAot(
        "module M; "
        "type L<T> := Array<T>; "
        "public interface ISink "
        "  method Show(xs: L<int>); "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Show(xs: Array<int>) "
        "    Console.WriteLine(\"ran-\" + xs.Count.ToString()); "
        "  end Show; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink := new Feeder(); "
        "    s.Show([1, 2]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran-2\n"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           nested_alias_argument_runs_spec_27_8) {
    // §27.8: `Array<A>` against `Array<Array<int>>`.
    auto r = runAot(
        "module M; "
        "type A := Array<int>; "
        "public interface ISink "
        "  method Show(xs: Array<A>); "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Show(xs: Array<Array<int>>) "
        "    Console.WriteLine(\"ran-\" + xs.Count.ToString()); "
        "  end Show; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink := new Feeder(); "
        "    s.Show([[1], [2]]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ran-2\n"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           alias_of_builtin_class_runs_spec_27_8) {
    // §27.8: `type M := Mutex` against `Mutex`.
    auto r = runAot(
        "module M; "
        "type M := Mutex; "
        "public interface ISink "
        "  method Show(xs: M); "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Show(xs: Mutex) "
        "    Console.WriteLine(\"mutex\"); "
        "  end Show; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink := new Feeder(); "
        "    s.Show(new Mutex()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("mutex\n"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           alias_in_return_type_runs_spec_27_8) {
    // §27.8: alias in the return type.
    auto r = runAot(
        "module M; "
        "type A := Array<int>; "
        "public interface ISink "
        "  method Show(): A; "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Show(): Array<int> "
        "    return [1, 2, 3]; "
        "  end Show; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink := new Feeder(); "
        "    Console.WriteLine(s.Show().Count.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           alias_to_different_array_rejected_spec_27_8) {
    // `type A := Array<string>` is not `Array<int>`.
    auto r = runAot(
        "module M; "
        "type A := Array<string>; "
        "public interface ISink "
        "  method Show(xs: A); "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Show(xs: Array<int>) "
        "     "
        "  end Show; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasConformanceDiagnostic(
        r, "method `Show` does not match interface `ISink`'s member `Show`"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           alias_to_exception_vs_stringbuilder_rejected_spec_27_8) {
    // `type E := Exception` is not `StringBuilder`.
    auto r = runAot(
        "module M; "
        "type E := Exception; "
        "public interface ISink "
        "  method Show(xs: E); "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Show(xs: StringBuilder) "
        "     "
        "  end Show; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasConformanceDiagnostic(
        r, "method `Show` does not match interface `ISink`'s member `Show`"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           alias_to_mutex_vs_random_rejected_spec_27_8) {
    // `type M := Mutex` is not `Random`.
    auto r = runAot(
        "module M; "
        "type M := Mutex; "
        "public interface ISink "
        "  method Show(xs: M); "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Show(xs: Random) "
        "     "
        "  end Show; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasConformanceDiagnostic(
        r, "method `Show` does not match interface `ISink`'s member `Show`"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           nullable_widening_on_builtin_class_runs_spec_11_2) {
    // §11.2 rule 1: a parameter may widen T -> T?; `Mutex` against `Mutex?`.
    auto r = runAot(
        "module M; "
        "public interface ISink "
        "  method Show(m: Mutex); "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Show(m: Mutex?) "
        "    Console.WriteLine(\"widened\"); "
        "  end Show; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink := new Feeder(); "
        "    s.Show(new Mutex()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("widened\n"));
}

BEZEL_TEST(iface_conformance_builtin_class_param_5996,
           nullable_narrowing_on_builtin_class_rejected_spec_11_2) {
    // The reverse (`Mutex?` -> `Mutex`) is not admitted.
    auto r = runAot(
        "module M; "
        "public interface ISink "
        "  method Show(m: Mutex?); "
        "end ISink; "
        "public class Feeder: ISink "
        "  public constructor() end constructor; "
        "  public method Show(m: Mutex) "
        "  end Show; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasConformanceDiagnostic(
        r, "method `Show` does not match interface `ISink`'s member `Show`"));
}
