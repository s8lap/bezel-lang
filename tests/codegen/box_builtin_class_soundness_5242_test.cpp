// fix #5242 (§14.1 "Generic declarations", v0.313): a generic record, choice
// or class instantiated with a BUILTIN class (`Box<Stopwatch>`,
// `Box<StringBuilder>`, `Box<CancellationTokenSource>`) was laid out as the
// ERASED template (T -> the i32 placeholder), so the 8-byte builtin pointer was
// stored into / read from a 4-byte slot: ASan stack/heap-buffer-overflow.
//
// Root cause: the `allUnbound` erasure test — open-coded in lowerNew
// (LlvmBackend_Exprs2.cpp) and in lowerTypeRef's generic-record and
// generic-choice branches (LlvmBackend_Layout.cpp) — classified any
// single-part, declaration-less, arg-less Named ref outside the active
// substitution as an unbound type parameter. A builtin class has exactly that
// shape (it has no TypeSymbol declaration). The fix is ONE shared predicate,
// Emitter::isUnboundTemplateArg, that also excludes the reserved builtin
// names; every site calls it, so lowerNew and lowerTypeRef cannot disagree
// (#3143 — the earlier attempt patched lowerNew alone and a `Box<Stopwatch>`
// return type then failed IR verification; see the return-type row).
//
// Oracle: runAotChecked (ASan + LSan on the generated program) and the exit
// code Main returns (7 = every read saw the stored builtin object).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── `new Box<Stopwatch>` as an inferred local (the reduced record repro).
BEZEL_TEST(box_builtin_class_soundness_5242, record_box_stopwatch_field_read) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Diagnostics; "
        "record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  using var s := new Stopwatch(); "
        "  var b := new Box<Stopwatch>(s); "
        "  s.Start(); "
        "  var sw := b.value; "
        "  if sw.IsRunning then return 7; end; "
        "  return 1; "
        "end Main; end C;"), 7);
}

// ── another builtin class (stdlib §3.1 StringBuilder).
BEZEL_TEST(box_builtin_class_soundness_5242, record_box_stringbuilder_field_read) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Text; "
        "record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"hello\"); "
        "  var b := new Box<StringBuilder>(sb); "
        "  var g := b.value; "
        "  if g.Length > 0 then return 7; end; "
        "  return 1; "
        "end Main; end C;"), 7);
}

// ── another builtin class (CancellationTokenSource).
BEZEL_TEST(box_builtin_class_soundness_5242, record_box_cancellation_token_source_field_read) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Threading; "
        "record Box<T>(value: T); "
        "class C public static method Main(): int "
        "  using var cts := new CancellationTokenSource(); "
        "  var b := new Box<CancellationTokenSource>(cts); "
        "  var g := b.value; "
        "  if g.Token.IsCancellationRequested = false then return 7; end; "
        "  return 1; "
        "end Main; end C;"), 7);
}

// ── The issue's shape (a generic CLASS whose `T` field holds a Stopwatch), with the
// field declared `owned` and the value transferred with `move` so the row pins
// the layout defect alone; 200 constructions + reads + disposals. (The issue's
// literal spelling, a NON-owned `v: T`, additionally hits #5270 — a non-owned
// builtin-class field released by the container's disposer — which is a
// separate, non-generic defect.)
BEZEL_TEST(box_builtin_class_soundness_5242, class_box_owned_stopwatch_field_200x) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "import System.Diagnostics as D; "
        "class Box<T> "
        "  public owned v: T; "
        "  public constructor(owned x: T) v := x; end constructor; "
        "end Box; "
        "class P "
        "  public static method Main(): int "
        "    var n := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      i := i + 1; "
        "      var s := new Stopwatch(); s.Start(); "
        "      using var b := new Box<Stopwatch>(move s); "
        "      if b.v.IsRunning then n := n + 1; end; "
        "    end while; "
        "    if n = 200 then return 7; end; "
        "    return 1; "
        "  end Main; "
        "end P; "), 7);
}

// ── `Box<Stopwatch>` as a method RETURN type: lowerTypeRef (signature) and lowerNew
// (the returned value) must agree on the specialized layout (#3143).
BEZEL_TEST(box_builtin_class_soundness_5242, record_box_stopwatch_return_type) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Diagnostics; "
        "record Box<T>(value: T); "
        "class C "
        "  public static method Make(s: Stopwatch): Box<Stopwatch> "
        "    return new Box<Stopwatch>(s); "
        "  end Make; "
        "  public static method Main(): int "
        "    using var s := new Stopwatch(); "
        "    var b := Make(s); "
        "    s.Start(); "
        "    var sw := b.value; "
        "    if sw.IsRunning then return 7; end; "
        "    return 1; "
        "  end Main; "
        "end C; "), 7);
}

// ── `Box<Stopwatch>` as a method PARAMETER type.
BEZEL_TEST(box_builtin_class_soundness_5242, record_box_stopwatch_param_type) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Diagnostics; "
        "record Box<T>(value: T); "
        "class C "
        "  public static method Read(b: Box<Stopwatch>): bool "
        "    return b.value.IsRunning; "
        "  end Read; "
        "  public static method Main(): int "
        "    using var s := new Stopwatch(); "
        "    s.Start(); "
        "    if Read(new Box<Stopwatch>(s)) then return 7; end; "
        "    return 1; "
        "  end Main; "
        "end C; "), 7);
}

// ── `Box<Stopwatch>` as a class FIELD type.
BEZEL_TEST(box_builtin_class_soundness_5242, record_box_stopwatch_class_field_type) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Diagnostics; "
        "record Box<T>(value: T); "
        "class H "
        "  public b: Box<Stopwatch>; "
        "  public constructor(x: Box<Stopwatch>) b := x; end constructor; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    using var s := new Stopwatch(); "
        "    var h := new H(new Box<Stopwatch>(s)); "
        "    s.Start(); "
        "    if h.b.value.IsRunning then return 7; end; "
        "    return 1; "
        "  end Main; "
        "end C; "), 7);
}

// ── `Box<Stopwatch>` as an annotated LOCAL type.
BEZEL_TEST(box_builtin_class_soundness_5242, record_box_stopwatch_annotated_local) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Diagnostics; "
        "record Box<T>(value: T); "
        "class C "
        "  public static method Main(): int "
        "    using var s := new Stopwatch(); "
        "    var b: Box<Stopwatch> := new Box<Stopwatch>(s); "
        "    s.Start(); "
        "    if b.value.IsRunning then return 7; end; "
        "    return 1; "
        "  end Main; "
        "end C; "), 7);
}

// ── The generic-CHOICE twin: `Opt<Stopwatch>` as a RETURN and a PARAMETER type
// (lowerTypeRef's choice branch shares the predicate; pre-fix the signature was
// the erased `{i32 tag, i32}` choice struct and IR verification failed).
BEZEL_TEST(box_builtin_class_soundness_5242, choice_opt_stopwatch_return_and_param) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Diagnostics; "
        "choice Opt<T> Some(v: T), None; end Opt; "
        "class C "
        "  public static method Make(s: Stopwatch): Opt<Stopwatch> "
        "    return Opt<Stopwatch>.Some(s); "
        "  end Make; "
        "  public static method Running(o: Opt<Stopwatch>): bool "
        "    return (case o of Some(var w) => w.IsRunning, _ => false); "
        "  end Running; "
        "  public static method Main(): int "
        "    using var s := new Stopwatch(); "
        "    s.Start(); "
        "    var o := Make(s); "
        "    if Running(o) then return 7; end; "
        "    return 1; "
        "  end Main; "
        "end C; "), 7);
}

// ── CONTROL (#732 / #3143): `new Box<T>` inside the template body of `Wrap<T>`
// keeps the erased layout (every arg genuinely unbound), and the Stopwatch and
// int instantiations of the caller both read back correctly.
BEZEL_TEST(box_builtin_class_soundness_5242, control_generic_method_template_body_unbound) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Diagnostics; "
        "record Box<T>(value: T); "
        "class C "
        "  public static method Wrap<T>(x: T): Box<T> "
        "    return new Box<T>(x); "
        "  end Wrap; "
        "  public static method Main(): int "
        "    using var s := new Stopwatch(); "
        "    s.Start(); "
        "    var b := Wrap<Stopwatch>(s); "
        "    var k := Wrap<int>(5); "
        "    if b.value.IsRunning and k.value = 5 then return 7; end; "
        "    return 1; "
        "  end Main; "
        "end C; "), 7);
}

// ── CONTROL (#5249 / §14.1 type-parameter shadowing): a type PARAMETER spelled
// like a builtin (`Holder<Stopwatch>`, `Wrap2<Stopwatch>`) used as a generic
// argument inside its own template body. The builtin-name exclusion now makes
// that arg "concrete" at EVERY site alike (the shared predicate), so the
// template signature and body still agree, and the int / long instantiations
// read back their own values.
BEZEL_TEST(box_builtin_class_soundness_5242, control_type_param_named_like_builtin_as_generic_arg) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Diagnostics; "
        "record Box<T>(value: T); "
        "class Holder<Stopwatch> "
        "  public static method Wrap(x: Stopwatch): Box<Stopwatch> "
        "    return new Box<Stopwatch>(x); "
        "  end Wrap; "
        "end Holder; "
        "class C "
        "  public static method Wrap2<Stopwatch>(x: Stopwatch): Box<Stopwatch> "
        "    return new Box<Stopwatch>(x); "
        "  end Wrap2; "
        "  public static method Main(): int "
        "    using var s := new Stopwatch(); "
        "    s.Start(); "
        "    var a := Wrap2<int>(5); "
        "    var b := Holder<int>.Wrap(2); "
        "    var c := Wrap2<long>(100000000000L); "
        "    if a.value = 5 and b.value = 2 and c.value = 100000000000L then return 7; end; "
        "    return 1; "
        "  end Main; "
        "end C; "), 7);
}
