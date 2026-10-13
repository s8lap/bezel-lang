// fix #5985 / spec §11.2 (v0.338): a member matches an interface member when
// its parameter types are "exactly equal under the interface's substitution".
// A generic built-in (`Array<V>`, `Dictionary<K, V>`) is not a bare type
// parameter, so its generic arguments must match under the substitution:
// `Array<string>` is not a witness for `Array<V>` at `V := int`. The bare
// method-level positional rule (`Show<U>` vs `Show<T>`) covers only a member's
// own type parameters (§11.2, *Generic members*, L1849).
//
// Reject rows are the oracle: before the fix the issue repro compiled and
// dumped core at run time. Reject rows assert `!r.ran` and the §11.2 needle,
// never `hasErrors()` alone. Run rows assert `r.ran` and the program's stdout.

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

BEZEL_TEST(iface_conformance_generic_arg_mismatch_5985,
           array_of_string_for_array_of_int_rejected_spec_11_2) {
    // §11.2 (v0.338): the issue repro. `Take(xs: Array<string>)` does not match
    // `Take(xs: Array<V>)` under `V := int`.
    auto r = runAot(
        "module M; "
        "public interface ISink<V> "
        "  method Take(xs: Array<V>); "
        "end ISink; "
        "public class Feeder: ISink<int> "
        "  public constructor() end constructor; "
        "  public method Take(xs: Array<string>) "
        "    for var x in xs do Console.WriteLine(x); end; "
        "  end Take; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var d := new Feeder(); "
        "    var s: ISink<int> := d; "
        "    s.Take([1, 2]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasConformanceDiagnostic(
        r, "method `Take` does not match interface `ISink`'s member `Take`"));
}

BEZEL_TEST(iface_conformance_generic_arg_mismatch_5985,
           dictionary_with_swapped_args_rejected_spec_11_2) {
    // §11.2 (v0.338): `Dictionary<int, string>` is not `Dictionary<K, V>` under
    // `K := string, V := int`; both arguments are compared.
    auto r = runAot(
        "module M; "
        "public interface IMap<K, V> "
        "  method Put(d: Dictionary<K, V>); "
        "end IMap; "
        "public class Store: IMap<string, int> "
        "  public constructor() end constructor; "
        "  public method Put(d: Dictionary<int, string>) "
        "  end Put; "
        "end Store; "
        "public class C "
        "  public static method Main(): int "
        "    var s: IMap<string, int> := new Store(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasConformanceDiagnostic(
        r, "method `Put` does not match interface `IMap`'s member `Put`"));
}

BEZEL_TEST(iface_conformance_generic_arg_mismatch_5985,
           nested_array_argument_mismatch_rejected_spec_11_2) {
    // §11.2 (v0.338): the mismatch sits one level down, inside `Array<Array<V>>`.
    auto r = runAot(
        "module M; "
        "public interface ISink<V> "
        "  method Take(xs: Array<Array<V>>); "
        "end ISink; "
        "public class Feeder: ISink<int> "
        "  public constructor() end constructor; "
        "  public method Take(xs: Array<Array<string>>) "
        "  end Take; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink<int> := new Feeder(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasConformanceDiagnostic(
        r, "method `Take` does not match interface `ISink`'s member `Take`"));
}

BEZEL_TEST(iface_conformance_generic_arg_mismatch_5985,
           unsubstituted_class_param_vs_concrete_rejected_spec_11_2) {
    // §11.2 (v0.338): `Gen<T>` implements `ISink<T>`, so the interface's `V` is
    // the class's `T`. A concrete `Array<int>` is not `Array<T>`.
    auto r = runAot(
        "module M; "
        "public interface ISink<V> "
        "  method Take(xs: Array<V>); "
        "end ISink; "
        "public class Gen<T>: ISink<T> "
        "  public constructor() end constructor; "
        "  public method Take(xs: Array<int>) "
        "  end Take; "
        "end Gen; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink<int> := new Gen<int>(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasConformanceDiagnostic(
        r, "method `Take` does not match interface `ISink`'s member `Take`"));
}

BEZEL_TEST(iface_conformance_generic_arg_mismatch_5985,
           method_level_generic_arg_concrete_rejected_spec_11_2) {
    // §11.2 (v0.338, *Generic members*): `Show<U>(xs: Array<U>)` corresponds to
    // `Show<T>(xs: Array<T>)` by position, not to `Array<int>`.
    auto r = runAot(
        "module M; "
        "public interface ISink<V> "
        "  method Show<U>(xs: Array<U>); "
        "end ISink; "
        "public class Feeder: ISink<int> "
        "  public constructor() end constructor; "
        "  public method Show<T>(xs: Array<int>) "
        "  end Show; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink<int> := new Feeder(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasConformanceDiagnostic(
        r, "method `Show` does not match interface `ISink`'s member `Show`"));
}

BEZEL_TEST(iface_conformance_generic_arg_mismatch_5985,
           array_of_int_for_array_of_int_runs_spec_11_2) {
    // §11.2 (v0.338): `Array<int>` under `V := int` matches; the call through
    // the interface reaches the implementation with the elements intact.
    auto r = runAot(
        "module M; "
        "public interface ISink<V> "
        "  method Take(xs: Array<V>); "
        "end ISink; "
        "public class Feeder: ISink<int> "
        "  public constructor() end constructor; "
        "  public method Take(xs: Array<int>) "
        "    for var x in xs do Console.WriteLine(x.ToString()); end; "
        "  end Take; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink<int> := new Feeder(); "
        "    s.Take([1, 2]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n2\n"));
}

BEZEL_TEST(iface_conformance_generic_arg_mismatch_5985,
           dictionary_with_matching_args_runs_spec_11_2) {
    // §11.2 (v0.338): `Dictionary<string, int>` under `K := string, V := int`
    // matches; the implementation sees the caller's dictionary.
    auto r = runAot(
        "module M; "
        "public interface IMap<K, V> "
        "  method Put(d: Dictionary<K, V>); "
        "end IMap; "
        "public class Store: IMap<string, int> "
        "  public constructor() end constructor; "
        "  public method Put(d: Dictionary<string, int>) "
        "    Console.WriteLine(d.Count.ToString()); "
        "  end Put; "
        "end Store; "
        "public class C "
        "  public static method Main(): int "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 1); "
        "    var s: IMap<string, int> := new Store(); "
        "    s.Put(d); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

BEZEL_TEST(iface_conformance_generic_arg_mismatch_5985,
           class_param_substituted_array_runs_spec_11_2) {
    // §11.2 (v0.338): `Gen<T>: ISink<T>` with `Take(xs: Array<T>)` is the same
    // instantiation on both sides and matches.
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
        "    s.Take([3, 4]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

BEZEL_TEST(iface_conformance_generic_arg_mismatch_5985,
           method_level_renamed_generic_arg_runs_spec_11_2) {
    // §11.2 (v0.338, *Generic members*): `Show<U>(xs: Array<U>)` implements
    // `Show<T>(xs: Array<T>)`; the bare parameters correspond by position.
    auto r = runAot(
        "module M; "
        "public interface ISink<V> "
        "  method Show<U>(xs: Array<U>); "
        "end ISink; "
        "public class Feeder: ISink<int> "
        "  public constructor() end constructor; "
        "  public method Show<T>(xs: Array<T>) "
        "    Console.WriteLine(xs.Count.ToString()); "
        "  end Show; "
        "end Feeder; "
        "public class C "
        "  public static method Main(): int "
        "    var s: ISink<int> := new Feeder(); "
        "    s.Show<int>([5, 6]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}
