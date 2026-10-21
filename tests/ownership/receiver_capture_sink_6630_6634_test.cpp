// Spec-derived tests for issue #6630 (and the settled part of #6634) —
// §28.3 (v0.340) *Announcement-erasing positions (the sink rule)*, for a
// closure that captures the RECEIVER of an instance member body.
//
// §28.3 (v0.340), the closure bullet: "while its source set is non-empty, a
// function value may not be stored in a field, in any collection, in a
// property or indexer, or through an `out` or `ref` parameter, may not be
// returned from the enclosing method". Provenance clause 1: a lambda literal
// contributes, for "a captured borrowed local or parameter, that binding's
// source". The receiver is such a binding:
//   * the §28.3 producers table's `this` row: Borrowed (source: caller) — a
//     CLASS receiver;
//   * §9.4 (*The receiver value is fixed at the call*): a copyable value
//     receiver's `this` is captured BY COPY and contributes "exactly what a
//     captured local of the receiver's type contributes" — with §9.7's
//     closure bullet ("record" read as "record or choice", §35.4), a value
//     receiver that HOLDS BORROWS contributes the sources of the borrows it
//     carries;
//   * §28.14 counts `this` itself among a closure's captured sources.
// So a lambda that reads such a `this` — explicitly, through `case this of
// …`, or through an implicit-`this` member reference (`_s`, `W`,
// `GetLabel()`) — has a non-empty set, and every sink above rejects it.
//
// NOT pinned here: a closure over the `this` of a value receiver whose type
// holds NO borrows (`record Pt(x: int)`, a payload-free or int-payload
// choice, an enum) at a sink. §9.4 (v0.340) admits it — the copy contributes
// nothing, so the closure is capture-free — and it is safe to run only once
// codegen copies the receiver into the closure's capture cell; tracker
// #6721 pins it with that change.
//
// Before the fix none of these were rejected: the record/class shapes
// compiled clean and, at runtime, read a dead receiver (#6630: a
// heap-use-after-free through a collection slot). The choice shapes reached
// codegen and died in the IR verifier (#6318), so for them a crash and a
// rejection both read `!r.ran` — every reject row below asserts the
// diagnostic TEXT.
//
// ACCEPT rows pin the other half: a receiver-capturing lambda that never
// reaches a sink (called locally, passed to a parameter), a capture-free or
// static-member-only lambda stored in a collection, and a lambda over a
// COPIED local all stay legal.
//
// per §28.3 (v0.340) sink rule, §9.7, §35.4, §28.14

#include <string>

#include "support/aot_result_asserts.hpp"   // diagnosticMessageContains
#include "support/aot_support.hpp"          // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

// The sink-rule citation every receiver-capture rejection carries.
static const char* kSinkRule = "(§28.3 sink rule, v0.340";

static const char* kWidget =
    "module M; "
    "public class Widget "
    "  private _s: string; "
    "  public constructor(s: string) _s := s; end constructor; "
    "  public method GetLabel(): string => _s; "
    "end Widget; ";

// ===========================================================================
// REJECT — the issues' own repros.
// ===========================================================================

// #6630's repro, record receiver: `this.W.GetLabel()` captured in a lambda
// stored into the caller's array. MEASURED before: rc 0 (accepted). `W` is an
// auto-property, which the shared §9.7 classifier does not read (#6708); the
// receiver predicate reads it.
// per §28.3 (v0.340) sink rule ("in any collection"), §9.4
BEZEL_TEST(receiver_capture_sink_6630_6634, record_this_collection_add_6630) {
    std::string src = std::string(kWidget) +
        "public record Held "
        "  public readonly property W: Widget; "
        "  public constructor(w: Widget) W := w; end constructor; "
        "  public method Hook(a: Array<() -> string>) "
        "    a.Add(lambda(): string => this.W.GetLabel()); "
        "  end Hook; "
        "end Held; "
        "public class Program "
        "  public static method Main() "
        "    var a := new Array<() -> string>(); "
        "    using var w := new Widget(\"hi\"); "
        "    var h := new Held(w); "
        "    h.Hook(a); "
        "    Console.WriteLine(a[0]()); "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "closure stored in a collection (`.Add(...)`) captures `this`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "the copy carries the borrows that value holds (§9.4, §9.7)"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSinkRule));
}

// #6630's repro, choice receiver: `case this of …` in the stored lambda.
// MEASURED before: the IR verifier error of #6318 ("does not have
// terminator"), i.e. sema and ownership ADMITTED it.
// per §28.3 (v0.340) sink rule, §9.4
BEZEL_TEST(receiver_capture_sink_6630_6634, choice_case_this_collection_add_6630) {
    std::string src = std::string(kWidget) +
        "public choice Held "
        "  One(w: Widget), "
        "  Nothing; "
        "  public method Hook(a: Array<() -> string>) "
        "    a.Add(lambda(): string => (case this of One(x) => x.GetLabel(), "
        "Nothing => \"none\")); "
        "  end Hook; "
        "end Held; "
        "public class Program "
        "  public static method Main() "
        "    var a := new Array<() -> string>(); "
        "    using var w := new Widget(\"hi\"); "
        "    var h := Held.One(w); "
        "    h.Hook(a); "
        "    Console.WriteLine(a[0]()); "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "closure stored in a collection (`.Add(...)`) captures `this`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "instance member of a choice"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSinkRule));
}

// ===========================================================================
// REJECT — the neighbouring receiver kinds, spellings and sinks.
// ===========================================================================

// #6630's record receiver at the RETURN sink, through an implicit-`this`
// component read: a positional `W: Widget` makes the record borrow-bearing
// (§9.7), so the returned closure carries the caller's Widget borrow.
// MEASURED before: rc 0.
// per §28.3 (v0.340) sink rule, §9.7 closure bullet
BEZEL_TEST(receiver_capture_sink_6630_6634, borrow_bearing_record_implicit_this_returned) {
    std::string src = std::string(kWidget) +
        "public record Held(W: Widget) "
        "  public method Maker(): () -> string "
        "    return lambda(): string => W.GetLabel(); "
        "  end Maker; "
        "end Held; "
        "public class Program "
        "  public static method Main() "
        "    using var w := new Widget(\"hi\"); "
        "    var f := new Held(w).Maker(); "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "closure returned from the method captures the receiver (`W` reads `this`)"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "instance member of a record"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "the copy carries the borrows that value holds (§9.4, §9.7)"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSinkRule));
}

// #6630's choice receiver at the RETURN sink: a `Widget` payload makes the
// choice hold borrows (§35.4). MEASURED before: the #6318 IR verifier error.
// per §28.3 (v0.340) sink rule, §9.7 closure bullet, §35.4
BEZEL_TEST(receiver_capture_sink_6630_6634, borrow_bearing_choice_case_this_returned) {
    std::string src = std::string(kWidget) +
        "public choice Held "
        "  One(w: Widget), "
        "  Nothing; "
        "  public method Maker(): () -> string "
        "    return lambda(): string => (case this of One(x) => x.GetLabel(), "
        "Nothing => \"none\"); "
        "  end Maker; "
        "end Held; "
        "public class Program "
        "  public static method Main() "
        "    using var w := new Widget(\"hi\"); "
        "    var f := Held.One(w).Maker(); "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "closure returned from the method captures `this`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "instance member of a choice"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSinkRule));
}

// #6630's record receiver through an `out` parameter, implicit `this`.
// MEASURED before: rc 0.
// per §28.3 (v0.340) sink rule ("through an `out` or `ref` parameter"), §9.7
BEZEL_TEST(receiver_capture_sink_6630_6634, borrow_bearing_record_this_out_param) {
    std::string src = std::string(kWidget) +
        "public record Held(W: Widget) "
        "  public method Put(out f: () -> string) "
        "    f := lambda(): string => W.GetLabel(); "
        "  end Put; "
        "end Held; "
        "public class Program "
        "  public static method Main() "
        "    using var w := new Widget(\"hi\"); "
        "    var g: () -> string; "
        "    new Held(w).Put(out g); "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "closure written through the `out` parameter `f` captures the receiver (`W` reads `this`)"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSinkRule));
}

// Class receiver, collection sink. MEASURED before: rc 0.
// per §28.3 (v0.340) sink rule, producers table `this` row
BEZEL_TEST(receiver_capture_sink_6630_6634, class_this_collection_add) {
    std::string src =
        "module M; "
        "public class Holder "
        "  public method GetLabel(): string => \"x\"; "
        "  public method Hook(a: Array<() -> string>) "
        "    a.Add(lambda(): string => this.GetLabel()); "
        "  end Hook; "
        "end Holder; "
        "public class Program "
        "  public static method Main() "
        "    var a := new Array<() -> string>(); "
        "    using var h := new Holder(); "
        "    h.Hook(a); "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "closure stored in a collection (`.Add(...)`) captures `this`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "instance member of a class"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSinkRule));
}

// Class receiver, field sink from a constructor body, implicit `this`.
// MEASURED before: rc 0.
// per §28.3 (v0.340) sink rule ("stored in a field"), §28.14
BEZEL_TEST(receiver_capture_sink_6630_6634, class_ctor_field_store_implicit_this) {
    std::string src =
        "module M; "
        "public class C "
        "  private _s: string; "
        "  private _f: () -> string; "
        "  public constructor(s: string) "
        "    _s := s; "
        "    _f := lambda(): string => _s; "
        "  end constructor; "
        "end C; "
        "public class Program "
        "  public static method Main() "
        "    using var c := new C(\"a\"); "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "closure stored in `_f` captures the receiver (`_s` reads `this`)"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSinkRule));
}

// Class receiver, field sink reached from inside a block-bodied lambda: the
// stored inner lambda still captures the METHOD's receiver. MEASURED before:
// rc 0.
// per §28.3 (v0.340) sink rule
BEZEL_TEST(receiver_capture_sink_6630_6634, class_field_store_inside_lambda_body) {
    std::string src =
        "module M; "
        "public class C "
        "  private _s: string; "
        "  private _f: () -> string; "
        "  public constructor(s: string) "
        "    _s := s; "
        "    _f := lambda(): string => \"x\"; "
        "  end constructor; "
        "  public method Arm() "
        "    var g := lambda() => do "
        "      _f := lambda(): string => _s; "
        "    end; "
        "    g(); "
        "  end Arm; "
        "end C; "
        "public class Program "
        "  public static method Main() "
        "    using var c := new C(\"a\"); "
        "    c.Arm(); "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "closure stored in `_f` captures the receiver (`_s` reads `this`)"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSinkRule));
}

// Class receiver, expression-bodied method returning the lambda.
// MEASURED before: rc 0.
// per §28.3 (v0.340) sink rule
BEZEL_TEST(receiver_capture_sink_6630_6634, class_expression_body_returned) {
    std::string src =
        "module M; "
        "public class C "
        "  private _s: string; "
        "  public constructor(s: string) _s := s; end constructor; "
        "  public method Maker(): () -> string => lambda(): string => _s; "
        "end C; "
        "public class Program "
        "  public static method Main() "
        "    using var c := new C(\"a\"); "
        "    var f := c.Maker(); "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "closure returned from the method captures the receiver (`_s` reads `this`)"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kSinkRule));
}

// ===========================================================================
// ACCEPT — no sink is reached, or nothing borrowed is captured.
// ===========================================================================

// A receiver-capturing lambda passed to a parameter (an argument position,
// not a sink) and one called locally, for both a class and a record.
// per §28.3 (v0.340) — the sink rule names only the positions above
BEZEL_TEST(receiver_capture_sink_6630_6634, accept_passed_as_argument_and_called_locally) {
    std::string src =
        "module M; "
        "public class C "
        "  private _s: string; "
        "  public constructor(s: string) _s := s; end constructor; "
        "  private static method Run(f: () -> string): string => f(); "
        "  public method Go(): string "
        "    return Run(lambda(): string => _s + \"!\"); "
        "  end Go; "
        "  public method Local(): string "
        "    var f := lambda(): string => this._s + \"?\"; "
        "    return f(); "
        "  end Local; "
        "end C; "
        "public record Pt(x: int) "
        "  private static method Run(f: () -> int): int => f(); "
        "  public method Go(): int => Run(lambda(): int => this.x + 1); "
        "  public method Local(): int "
        "    var f := lambda(): int => x * 2; "
        "    return f(); "
        "  end Local; "
        "end Pt; "
        "public class Program "
        "  public static method Main() "
        "    using var c := new C(\"a\"); "
        "    Console.WriteLine(c.Go()); "
        "    Console.WriteLine(c.Local()); "
        "    Console.WriteLine(new Pt(4).Go()); "
        "    Console.WriteLine(new Pt(4).Local()); "
        "  end Main; "
        "end Program;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a!\na?\n5\n8\n"));
}

// Capture-free and static-member-only lambdas stored into a collection and
// returned from an instance method: an empty source set (clause 1 — a static
// member names no receiver).
// per §28.3 (v0.340) sink rule ("a capture-free value is admitted")
BEZEL_TEST(receiver_capture_sink_6630_6634, accept_capture_free_and_static_member_stored) {
    std::string src =
        "module M; "
        "public class C "
        "  private static method Name(): string => \"n\"; "
        "  public method Maker(): () -> string "
        "    return lambda(): string => Name(); "
        "  end Maker; "
        "  public method Hook(a: Array<() -> string>) "
        "    a.Add(lambda(): string => \"k\"); "
        "    a.Add(lambda(): string => Name()); "
        "  end Hook; "
        "end C; "
        "public class Program "
        "  public static method Main() "
        "    using var c := new C(); "
        "    var a := new Array<() -> string>(); "
        "    c.Hook(a); "
        "    var f := c.Maker(); "
        "    Console.WriteLine(f() + a[0]() + a[1]()); "
        "  end Main; "
        "end Program;";
    // runAot, not runAotChecked: a closure stored in an `Array<() -> T>` is
    // never released when the array is disposed (#6212, open, 64 B per stored
    // lambda under LSan), which is unrelated to the verdict this row pins.
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("nkn\n"));
}

// The issue's remedy: copy the receiver's data into a local and capture the
// copy — an `int` carries no borrow (clause 1), so the returned closure's set
// is empty. Called after the receiver is gone, it still reads 7.
// per §28.3 (v0.340) provenance clause 1, §9.4
BEZEL_TEST(receiver_capture_sink_6630_6634, accept_copied_component_returned) {
    std::string src =
        "module M; "
        "public record Pt(x: int) "
        "  public method Maker(): () -> int "
        "    var y := x; "
        "    return lambda(): int => y; "
        "  end Maker; "
        "end Pt; "
        "public class Program "
        "  private static method MakeFrom(): () -> int "
        "    var p := new Pt(7); "
        "    return p.Maker(); "
        "  end MakeFrom; "
        "  public static method Main() "
        "    var f := MakeFrom(); "
        "    Console.WriteLine(f()); "
        "  end Main; "
        "end Program;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}
