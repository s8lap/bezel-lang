// fix #7142 (§28.6 / §28.3, v0.343): `Dispose()` on a GETTER's result is a
// compile error — in BOTH the direct spelling and the method-group spelling.
//
// THE DEFECT. A getter's result is a BORROW: §28.3's producers table (v0.343)
// reads "Property access `obj.P` (instance getter) | Borrowed (source: `obj`)"
// and "Static property access `T.P` (class-typed result) | Borrowed (source:
// the declaring type's static storage)". §28.6's borrow-receiver list (v0.343)
// names "a getter or other non-`owned` call result" among the receivers on
// which "`Dispose()` or a direct `DisposeAsync()` call through a **borrow** is
// a compile error". The compiler ACCEPTED `h.GetD.Dispose()` and
// `var f := h.GetD.Dispose;` with no diagnostic at all.
//
// §28.6's A4 bullet is the signature-level backing: "properties and indexers
// do not carry `owned` (§23; the `owned` modifier applies only to fields), so
// its return is always a borrow" — a getter is a borrow-producing position.
// Accepting the disposal admits a double-dispose: the object also disposes
// the getter's result at its own scope end.
//
// §27.6's rule ("every receiver-side rule of the call the formation resolves
// as is read at the formation") is why BOTH spellings must reject: the
// formation reads the same §28.6 receiver rule as the call.
//
// THE CONTROLS. §28.6's borrow list has precise neighbours, and a fix that
// rejects a case the spec admits is as bad as the defect. Each control below
// is measured, not assumed:
//   1. a `Task`-family CALL result — §28.6 says in so many words that it is
//      an owned temporary and NOT in the borrow list;
//   2. an `owned` FIELD read inside the declaring class — §28.6 admits it;
//   3. an `owned` PARAMETER as the receiver — §28.6 admits it;
//   4. a fresh `new` result as the receiver — §28.6 admits it (an unbound
//      owned temporary);
//   5. a getter whose result is itself OWNED — not expressible in the
//      language: `owned` does not apply to a property's type (§28.1/§23), the
//      spelling is a PARSE error, and every getter that would hand back an
//      owned value is already rejected at its DEFINITION site by §28.6's A4
//      bullet (the getter-fresh-owned check). The row below pins that the
//      getter-fresh-owned definition-site rule still fires — the receiver
//      rule is not a substitute for it.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// The §28.6 getter-result receiver diagnostic — a stable, rule-specific
// fragment (the same shape the other §28.6 receiver rules use).
constexpr char kGetterResultDispose[] =
    "cannot call `Dispose()` on a getter result";

// The §28.6 A4 definition-site diagnostic — the getter-fresh-owned rule.
constexpr char kGetterFreshOwned[] =
    "a property or indexer getter may not return a freshly-produced owned "
    "value";

// The holder whose `GetD` getter reads an `owned` field: the getter's result
// is a borrow of the holder, not of the caller.
std::string program(const std::string& body) {
    return std::string("module M; "
        "import System; "
        "class D "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end D; "
        "class Holder "
        "  public owned d: D; "
        "  public constructor() d := new D(); end constructor; "
        "  public property GetD: D get => d; end GetD; "
        "end Holder; "
        "class C "
        "  public static method Main(): int ") + body +
        " end Main; "
        "end C;";
}

}  // namespace

// The DIRECT spelling: `h.GetD.Dispose()`. §28.6's receiver rule reads the
// receiver expression's ownership classification, and the getter's result is
// a borrow (§28.3's property-access row) — so the call is a compile error.
BEZEL_TEST(getter_result_dispose_receiver_7142,
           direct_dispose_on_a_getter_result_is_rejected_28_6) {
    auto r = parseAndCheckOwnership(program(
        "var h := new Holder(); "
        "h.GetD.Dispose(); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kGetterResultDispose));
    // §28.6's own §-citation is in the message, so the rule that fired is
    // the receiver rule and not an unrelated diagnostic.
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.6"));
}

// The METHOD-GROUP spelling: `var f := h.GetD.Dispose;`. §27.6: "every
// receiver-side rule of the call the formation resolves as is read at the
// formation" — so the SAME §28.6 receiver rule rejects this spelling, with
// the SAME diagnostic.
BEZEL_TEST(getter_result_dispose_receiver_7142,
           method_group_over_a_getter_result_is_rejected_28_6) {
    auto r = parseAndCheckOwnership(program(
        "var h := new Holder(); "
        "var f := h.GetD.Dispose; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kGetterResultDispose));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.6"));
}

// The getter's result is a borrow EVEN when the getter reads an `owned`
// field: §28.3's producers row reads "Property access `obj.P` (instance
// getter) | Borrowed (source: `obj`)" at every declared type, and §28.6's
// `owned`-field admission is a FIELD-read admission ("an `owned` field —
// ... read by code of the owning object's declaring type"), not a
// property-read one. So the disposal through the getter is rejected even
// inside the declaring class, while the disposal through the FIELD
// (the control below) is admitted.
BEZEL_TEST(getter_result_dispose_receiver_7142,
           getter_over_an_owned_field_is_still_a_borrow_28_3) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "import System; "
        "class D "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end D; "
        "class Holder "
        "  public owned d: D; "
        "  public constructor() d := new D(); end constructor; "
        "  public property GetD: D get => d; end GetD; "
        "  public method Release() "
        "    this.GetD.Dispose(); "
        "  end Release; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    h.Release(); "
        "    return 0; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kGetterResultDispose));
}

// CONTROL 1 — a `Task`-family CALL result is an owned temporary, and §28.6
// says in so many words that it is NOT in the borrow list ("a `Task`-family
// call result is an owned temporary — §28.3 — and is not in this list"). The
// receiver rule must leave it alone.
BEZEL_TEST(getter_result_dispose_receiver_7142,
           task_family_call_result_receiver_is_admitted_28_6) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "import System; "
        "class C "
        "  public static method Main(): int "
        "    Task.FromResult(42).Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL 2 — an `owned` FIELD read inside the declaring class: §28.6
// admits it ("an `owned` field — ... read by code of the owning object's
// declaring type"). The receiver rule must leave it alone.
BEZEL_TEST(getter_result_dispose_receiver_7142,
           owned_field_read_inside_the_declaring_class_is_admitted_28_6) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "import System; "
        "class D "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end D; "
        "class Holder "
        "  public owned d: D; "
        "  public constructor() d := new D(); end constructor; "
        "  public method Release() "
        "    this.d.Dispose(); "
        "  end Release; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    h.Release(); "
        "    return 0; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL 3 — an `owned` PARAMETER as the receiver: §28.6 admits it ("an
// owned local or `owned` parameter"). The receiver rule must leave it alone.
BEZEL_TEST(getter_result_dispose_receiver_7142,
           owned_parameter_receiver_is_admitted_28_6) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "import System; "
        "class D "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end D; "
        "class C "
        "  public static method Take(owned d: D) "
        "    d.Dispose(); "
        "  end Take; "
        "  public static method Main(): int "
        "    Take(new D()); "
        "    return 0; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL 4 — a fresh `new` result as the receiver: §28.6 admits it (an
// unbound owned temporary, §28.3). The receiver rule must leave it alone.
BEZEL_TEST(getter_result_dispose_receiver_7142,
           fresh_new_result_receiver_is_admitted_28_6) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "import System; "
        "class D "
        "  public constructor() end constructor; "
        "  destructor Console.WriteLine(\"disposed\"); end destructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    new D().Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL 5 — a getter whose result is itself OWNED is not expressible in
// the language, and the DEFINITION-site rule that says so still fires. This
// pins that the receiver rule did not displace §28.6's A4 bullet: a getter
// that would hand back a fresh owned value is rejected where it is written.
BEZEL_TEST(getter_result_dispose_receiver_7142,
           getter_returning_a_fresh_owned_value_is_rejected_at_its_definition_28_6) {
    auto r = parseAndCheckOwnership(std::string("module M; "
        "import System; "
        "class D "
        "  public constructor() end constructor; "
        "end D; "
        "class Holder "
        "  public property GetD: D get => new D(); end GetD; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kGetterFreshOwned));
}
