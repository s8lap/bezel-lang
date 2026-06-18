// fix #773 / §26.5: extension blocks on user-defined GENERIC class types are
// matched against receivers of that type and dispatch correctly.
//
// §26.5: a generic extension `extension Bag<T> where T: class` applies to any
// `Bag<X>` — the free type variable `T` unifies with the receiver's argument.
// Previously the extension-target matcher deferred generic targets entirely, so
// `b.IsEmpty()` on a `Bag<Widget>` reported "no member IsEmpty" (FALSE_REJECT).
//
// SCOPE: a method whose SIGNATURE references the free type variable (`FirstOr(x:
// T): T`) needs per-instantiation T-substitution in codegen, not yet
// implemented; such a method is kept cleanly REJECTED (a "no member"
// diagnostic) rather than matched into a type-mismatched call (IR_FAIL). A
// method that uses T only via the receiver's own members (`IsEmpty(): bool`) is
// fully supported.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
bool rejected(const std::string& program) {
    return !runJit(program).ran;
}

// `Bag<T>` (a generic class) + an `extension Bag<T>` declaring `body`.
std::string prog(const std::string& ext, const std::string& mainBody) {
    return std::string(
        "module M; "
        "class Bag<T> where T: class "
        "  private _items: Array<T>; "
        "  public constructor() _items := new Array<T>(); end; "
        "  public method Add(x: T): () _items.Add(x); end; "
        "  public method Count(): int => _items.Count; "
        "end Bag; "
        "class W public v: int; public constructor(n: int) v := n; end; end W; ") +
        ext +
        " public static class P public static method Main(): int " + mainBody +
        " end Main; end P;";
}
}  // namespace

// §26.5: a generic extension's no-free-var method dispatches on a concrete
// receiver. `IsEmpty()` uses T only via the receiver's `Count()`. (The repro.)
BEZEL_TEST(generic_extension_773, no_free_var_method_dispatches) {
    BEZEL_EXPECT_EQ(runFull(prog(
        "public extension Bag<T> where T: class "
        "  public method IsEmpty(): bool => this.Count() = 0; "
        "end; ",
        "  using var b := new Bag<W>(); "
        "  return 0 if b.IsEmpty() else 1; ")), 0);  // empty -> IsEmpty true
}

// §26.5: after adding an element, the extension method observes the receiver's
// updated state (dispatch is on the live receiver, not a stale copy).
BEZEL_TEST(generic_extension_773, method_sees_receiver_state) {
    BEZEL_EXPECT_EQ(runFull(prog(
        "public extension Bag<T> where T: class "
        "  public method IsEmpty(): bool => this.Count() = 0; "
        "end; ",
        "  using var b := new Bag<W>(); "
        "  b.Add(new W(1)); "
        "  return 0 if not b.IsEmpty() else 1; ")), 0);  // non-empty
}

// §26.5 SCOPE: a method whose signature uses the free var is kept cleanly
// rejected (not IR_FAIL) — the T-substitution case is deferred.
BEZEL_TEST(generic_extension_773, free_var_in_signature_rejected_cleanly) {
    BEZEL_EXPECT_TRUE(rejected(prog(
        "public extension Bag<T> where T: class "
        "  public method FirstOr(fallback: T): T => fallback; "
        "end; ",
        "  using var b := new Bag<W>(); "
        "  using var f := new W(9); "
        "  var r := b.FirstOr(f); "
        "  return r.v; ")));
}

// §26.5 safety: a CONCRETE generic extension does not apply to a different
// instantiation — `extension Box<W>` is not seen on a `Box<A>` receiver.
BEZEL_TEST(generic_extension_773, concrete_generic_extension_not_misapplied) {
    BEZEL_EXPECT_TRUE(rejected(
        "module M; "
        "class Box<T> where T: class public _v: T; "
        "  public constructor(v: T) _v := v; end; end Box; "
        "class W public v: int; public constructor(n: int) v := n; end; end W; "
        "class A public n: int := 0; end A; "
        "public extension Box<W> "
        "  public method Tag(): int => 7; "
        "end; "
        "public static class P public static method Main(): int "
        "  using var b := new Box<A>(new A()); "
        "  return b.Tag(); "
        "end Main; end P;"));
}
