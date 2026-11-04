// Regression tests for issue #8139: the no-selector Array<T>.Min()/Max() over
// a record whose IComparable<T> conformance comes ONLY through an interface
// that EXTENDS IComparable<T> must be admitted.
//
//   * stdlib §5.1 (v0.355): `public extension IEnumerable<T> where T:
//     IComparable<T>` declares the no-selector `Min(): T` / `Max(): T`.
//   * spec §11 "Interface conformance" rule 2 (v0.355): "T is a record that
//     lists I (or any interface extending I) in its inheritance clause and
//     declares matching members".
//   * spec §14.2 (v0.355): a record conforming to the constraint interface
//     satisfies `where T: IFoo`.
//
// Controls: no IComparable at all, and IComparable of a different T, stay
// refused (stdlib §2.8, `CompareTo(other: T)`).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

std::string out(const std::string& program) {
    auto r = runAot(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

bool refusedWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

const char* kMain =
    "public class C "
    "public static method Main(): int "
    "var a: Array<Pt> := [new Pt(3), new Pt(1), new Pt(2)]; "
    "Console.WriteLine(a.Min().x); Console.WriteLine(a.Max().x); "
    "return 0; end Main; end C;";

const char* kCmp =
    "method CompareTo(other: Pt): int => "
    "(if x > other.x then 1 else (if x < other.x then -1 else 0)); ";

}  // namespace

// The issue's repro: one level of extension.
BEZEL_TEST(minmax_transitive_icomparable_8139, one_level_via_generic_interface) {
    BEZEL_EXPECT_EQ(
        out(std::string("module M; public interface IOrd<T>: IComparable<T> "
                        "end IOrd; "
                        "record Pt(x: int): IOrd<Pt> public ") + kCmp +
            "end Pt; " + kMain),
        "1\n3\n");
}

// Two levels of extension.
BEZEL_TEST(minmax_transitive_icomparable_8139, two_levels_via_generic_interfaces) {
    BEZEL_EXPECT_EQ(
        out(std::string("module M; public interface IOrd<T>: IComparable<T> "
                        "end IOrd; "
                        "public interface IOrd2<U>: IOrd<U> end IOrd2; "
                        "record Pt(x: int): IOrd2<Pt> public ") + kCmp +
            "end Pt; " + kMain),
        "1\n3\n");
}

// A non-generic extending interface fixing the argument.
BEZEL_TEST(minmax_transitive_icomparable_8139, non_generic_extending_interface) {
    BEZEL_EXPECT_EQ(
        out(std::string("module M; public interface IPtOrd: IComparable<Pt> "
                        "end IPtOrd; "
                        "record Pt(x: int): IPtOrd public ") + kCmp +
            "end Pt; " + kMain),
        "1\n3\n");
}

// Control: the direct form (admitting twin; green before and after).
BEZEL_TEST(minmax_transitive_icomparable_8139, control_direct_icomparable) {
    BEZEL_EXPECT_EQ(
        out(std::string("module M; record Pt(x: int): IComparable<Pt> public ") +
            kCmp + "end Pt; " + kMain),
        "1\n3\n");
}

// Reject control: no IComparable anywhere.
BEZEL_TEST(minmax_transitive_icomparable_8139, reject_no_icomparable) {
    BEZEL_EXPECT_TRUE(refusedWith(
        std::string("module M; public interface IOrd<T> end IOrd; "
                    "record Pt(x: int): IOrd<Pt> end Pt; ") + kMain,
        "Pt does not implement IComparable<T>"));
}

// Reject control: the extending interface reaches IComparable of a DIFFERENT T.
BEZEL_TEST(minmax_transitive_icomparable_8139, reject_icomparable_of_other_type) {
    BEZEL_EXPECT_TRUE(refusedWith(
        std::string("module M; public record B(y: int); "
                    "public interface IOrd<T>: IComparable<T> end IOrd; "
                    "record Pt(x: int): IOrd<B> "
                    "public method CompareTo(other: B): int => x - other.y; "
                    "end Pt; ") + kMain,
        "Pt does not implement IComparable<T>"));
}
