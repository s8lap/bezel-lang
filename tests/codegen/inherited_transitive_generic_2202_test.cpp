// §12.7/§12.9/§14.1/§19.3 (#2202, spec v0.226) — inherited methods of a
// generic base must receive the fully composed ancestor substitution when a
// derived receiver is instantiated through an enclosing generic.  In
// `Outer<X> -> Sub<X> -> Base<X>`, X is still open in the receiver TypeRef but
// is concrete in the active monomorphization environment.  The runtime type
// test therefore uses that concrete argument, per instantiation.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool rejectsWithCollapse(const std::string& src) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not runtime-distinguishable") != std::string::npos &&
            d.message.find("#2069") != std::string::npos)
            return true;
    return false;
}
}  // namespace

// False-FALSE direction: the reified Base<T=int> descriptor must match Wrap<int>
// and must not match its Wrap<string> sibling.  Before #2202 both missed because
// the inherited method saw the still-open Outer.X.  Encoded as 2*match+sibling.
BEZEL_TEST(inherited_transitive_generic_2202,
           inherited_base_is_reified_matches_precisely_14_1_19_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Wrap<W> public tag: int; public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class Base<T> public method probe(m: Object, w: Object): int "
        "  => (if m is Wrap<T> then 1 else 0) * 2 + (if w is Wrap<T> then 1 else 0); end Base; "
        "class Sub<U>: Base<U> end Sub; "
        "class Outer<X> public method run(m: Object, w: Object): int "
        "  using var s := new Sub<X>(); return s.probe(m, w); end run; end Outer; "
        "class C public static method Main(): int "
        "  using var wi := new Wrap<int>(1); using var ws := new Wrap<string>(2); "
        "  var mi: Object := wi; var sx: Object := ws; using var o := new Outer<int>(); "
        "  return o.run(mi, sx); end Main; end C;"), 2);
}

// False-TRUE direction and memory-safety precursor: `as Wrap<T>` must not admit
// a collapsed function-type target.  Before #2202 this compiled and treated a
// Wrap<(string)->string> as Wrap<(int)->int>; composing T exposes the target to
// the §19.3/#2069 runtime-distinguishability guard, which must reject it.
BEZEL_TEST(inherited_transitive_generic_2202,
           inherited_base_as_erased_function_rejects_14_1_19_3) {
    BEZEL_EXPECT_TRUE(rejectsWithCollapse(
        "module M; "
        "class Wrap<W> public tag: int; public constructor(t: int) tag := t; end constructor; end Wrap; "
        "class Base<T> public method probe(o: Object): int "
        "  var x: Wrap<T>? := o as Wrap<T>; return (if x <> null then 7 else 9); end probe; end Base; "
        "class Sub<U>: Base<U> end Sub; "
        "class Outer<X> public method run(o: Object): int "
        "  using var s := new Sub<X>(); return s.probe(o); end run; end Outer; "
        "class C public static method Main(): int "
        "  using var w := new Wrap<(string) -> string>(1); var x: Object := w; "
        "  using var o := new Outer<(int) -> int>(); return o.run(x); end Main; end C;"));
}

// #585 control (§12/§14): inherited generic-base field layout and inherited
// method dispatch continue to compose T through a directly generic receiver.
BEZEL_TEST(inherited_transitive_generic_2202,
           control_585_generic_base_field_layout_12_14) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Base<T> public v: T; public constructor(x: T) v := x; end constructor; "
        "public method Get(): T => v; end Base; "
        "class Sub<U>: Base<U> public constructor(x: U) base(x); end constructor; end Sub; "
        "class C public static method Main(): int using var s := new Sub<int>(42); return s.Get(); end Main; end C;"), 42);
}

// #902 control (§12/§14.2): a concrete derived receiver with no own generic args
// still binds the inherited generic base method to Base<int>.
BEZEL_TEST(inherited_transitive_generic_2202,
           control_902_concrete_derived_inherited_method_12_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Base<T> public v: T; public constructor(x: T) v := x; end constructor; "
        "public method Get(): T => v; end Base; "
        "class IntSub: Base<int> public constructor(x: int) base(x); end constructor; end IntSub; "
        "class C public static method Main(): int using var s := new IntSub(43); return s.Get(); end Main; end C;"), 43);
}

// #747 control (§12.3/§14.1/§22.6): synthesized `this` receivers keep resolving
// inherited generic return types through the ancestor walk.
BEZEL_TEST(inherited_transitive_generic_2202,
           control_747_this_inherited_generic_return_12_3_14_1_22_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Base<T> public v: T; public constructor(x: T) v := x; end constructor; "
        "public method Get(): T => v; end Base; "
        "class Sub: Base<int> public constructor(x: int) base(x); end constructor; "
        "public method Plus(): int => this.Get() + 1; end Sub; "
        "class C public static method Main(): int using var s := new Sub(43); return s.Plus(); end Main; end C;"), 44);
}

// #1101-C control (§12/§14.1): a bare implicit-this call to an inherited generic
// method keeps the composed nullable class return type.
BEZEL_TEST(inherited_transitive_generic_2202,
           control_1101_c_bare_inherited_generic_call_12_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Item public n: int; public constructor(x: int) n := x; end constructor; end Item; "
        "class Base<T> public method Make(): T? => null; end Base; "
        "class Sub: Base<Item> public method Run(): Item? => Make(); end Sub; "
        "class C public static method Main(): int using var s := new Sub(); var x := s.Run(); "
        "return (if x = null then 45 else 0); end Main; end C;"), 45);
}
