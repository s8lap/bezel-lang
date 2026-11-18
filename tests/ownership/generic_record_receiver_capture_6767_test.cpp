// Regression tests for #6767: a GENERIC record whose member returns or stores
// a closure over `this` is judged at each instantiation, not only at its
// declaration.
//
// Spec (v0.359), §9.4 *The receiver value is fixed at the call*:
//   "For a generic value kind both counts are read at each instantiation
//    (§3.6 rule 2): at one whose classification is owning, a closure over
//    `this` that would outlive the activation is a requirement of the
//    constructed type, rejected at every site that names that type, as
//    §19.3's write-point rule is at a move-only substitution."
//   and for a copyable receiver, the copy contributes "the sources of the
//   borrows the value carries where it holds them ..., so the closure meets
//   the sink rule as any capture-bearing one does".
// §9.7: "Reference-typed fields in records are always borrows; the record does
//   not own what they point to." — so `Bx<Widget>`'s `V` is a borrow, while
//   `Bx<int>`'s is not.
// §3.6 rule 2: "A use of a generic type's parameter, in any member body ... is
//   therefore a requirement of the constructed type, checked at every site
//   that names it whether or not the program reaches that member", and through
//   a chain of generic declarations "the diagnostic landing at the first site
//   whose argument decides the requirement and naming the chain".
// §28.3 sink rule: "While its source set is non-empty, a function value may
//   not be stored in a field, in any collection, in a property or indexer, or
//   through an `out` or `ref` parameter, may not be returned from the
//   enclosing method".
//
// Before the fix the receiver-capture leg read the generic declaration only,
// where `V: T` holds no borrow, so every row below was ACCEPTED (repro 1 ran
// and printed `M.Widget`). The admitting twins (`Bx<int>`, `Bx<string>`, a
// capture-free `Maker`, a closure invoked in the body, `Bx<Bx<int>>`) are
// corpus rows; `Bx<int>` / `Bx<string>` are also pinned by
// tests/codegen/value_receiver_this_capture_by_copy_6721_test.cpp.
//
// per §3.6, §9.4, §9.7, §28.3 (v0.359)

#include <string>

#include "support/aot_result_asserts.hpp"   // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// Lines 1-6 of every program below.
const char* kWidget6767 =
    "module M;\n"
    "public class Widget\n"
    "  private _s: string;\n"
    "  public constructor(s: string) _s := s; end constructor;\n"
    "  public method GetLabel(): string => _s;\n"
    "end Widget;\n";

// Lines 7-11.
const char* kBx6767 =
    "public record Bx<T>(V: T)\n"
    "  public method Maker(): () -> string\n"
    "    return lambda(): string => V.ToString();\n"
    "  end Maker;\n"
    "end Bx;\n";

// Lines 7-11: the closure returns the component itself.
const char* kBt6767 =
    "public record Bt<T>(V: T)\n"
    "  public method Maker(): () -> T\n"
    "    return lambda(): T => V;\n"
    "  end Maker;\n"
    "end Bt;\n";

const char* kSiteNamesBxWidget = "this site names `Bx<Widget>`";
const char* kTypeArgWidget = "the type argument `Widget` for `T`";
}  // namespace

// #6767 repro 1, literal: the returned closure captures a copy of a
// `Bx<Widget>`, whose `V` is a borrow of `w` (§9.7), so its source set is
// non-empty and the return is a §28.3 sink. Rejected at the site that names
// `Bx<Widget>` (line 15), naming the type argument.
BEZEL_TEST(generic_record_receiver_capture_6767,
           returned_this_closure_rejected_at_reference_instantiation_spec_9_4) {
    auto r = runAot(std::string(kWidget6767) + kBx6767 +
        "public class Program\n"
        "  public static method Main()\n"
        "    using var w := new Widget(\"w\");\n"
        "    var f := new Bx<Widget>(w).Maker();\n"  // line 15
        "    Console.WriteLine(f());\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, ":15:"));
    BEZEL_EXPECT_TRUE(diagContains(r, kSiteNamesBxWidget));
    BEZEL_EXPECT_TRUE(diagContains(r, kTypeArgWidget));
    BEZEL_EXPECT_TRUE(diagContains(r, "returned from the method"));
}

// The closure STORED in a collection, not returned: the same sink rule
// ("in any collection"), the same per-instantiation requirement.
BEZEL_TEST(generic_record_receiver_capture_6767,
           stored_this_closure_rejected_at_reference_instantiation_spec_9_4) {
    auto r = runAot(std::string(kWidget6767) +
        "public record Bx<T>(V: T)\n"
        "  public method Register(fs: Array<() -> string>)\n"
        "    fs.Add(lambda(): string => V.ToString());\n"
        "  end Register;\n"
        "end Bx;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    using var w := new Widget(\"w\");\n"
        "    var fs := new Array<() -> string>();\n"
        "    new Bx<Widget>(w).Register(fs);\n"  // line 16
        "    Console.WriteLine(fs[0]());\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, ":16:"));
    BEZEL_EXPECT_TRUE(diagContains(r, kSiteNamesBxWidget));
    BEZEL_EXPECT_TRUE(diagContains(r, kTypeArgWidget));
    BEZEL_EXPECT_TRUE(diagContains(r, "stored in a collection"));
}

// The closure formed through a GENERIC CALLER `M<U>(b: Bt<U>)`, instantiated
// at `Widget`: the constructed type `Bt<Widget>` is named at the argument
// (line 16), and §3.6 rule 2 checks the requirement there. (`Bt`'s closure
// returns `V` itself, not `V.ToString()`, which a bare-`U` receiver does not
// yet lower — #5953 — and would reject the program for an unrelated reason.)
BEZEL_TEST(generic_record_receiver_capture_6767,
           this_closure_through_generic_caller_rejected_spec_9_4_3_6) {
    auto r = runAot(std::string(kWidget6767) + kBt6767 +
        "public class Program\n"
        "  private static method M<U>(b: Bt<U>): () -> U => b.Maker();\n"
        "  public static method Main()\n"
        "    using var w := new Widget(\"w\");\n"
        "    var f := M<Widget>(new Bt<Widget>(w));\n"  // line 16
        "    Console.WriteLine(f().GetLabel());\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, ":16:"));
    BEZEL_EXPECT_TRUE(diagContains(r, "this site names `Bt<Widget>`"));
    BEZEL_EXPECT_TRUE(diagContains(r, kTypeArgWidget));
}

// A chain of generic TYPES (§3.6 rule 2): `Holder<U>`'s body names `Bx<U>`,
// so the requirement is decided at `Holder<Widget>` (line 22), "the first
// site whose argument decides the requirement", and the diagnostic names the
// chain.
BEZEL_TEST(generic_record_receiver_capture_6767,
           this_closure_through_generic_type_chain_rejected_spec_3_6_rule_2) {
    auto r = runAot(std::string(kWidget6767) + kBt6767 +
        "public class Holder<U>\n"
        "  public constructor() end constructor;\n"
        "  public method Make(u: U): int\n"
        "    var f := new Bt<U>(u).Maker();\n"
        "    return 1;\n"
        "  end Make;\n"
        "end Holder;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    using var w := new Widget(\"w\");\n"
        "    Console.WriteLine(new Holder<Widget>().Make(w));\n"  // line 22
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, ":22:"));
    BEZEL_EXPECT_TRUE(diagContains(r, "this site names `Bt<Widget>`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "`Holder`'s type parameter `U`"));
    BEZEL_EXPECT_TRUE(diagContains(r, kTypeArgWidget));
}
