// Codegen + sema — #959: the span value types are usable as TYPE ANNOTATIONS
// (parameter types, local annotations, record fields), not only inferred from
// `.AsSpan()` (stdlib §4.14-§4.15).
//
// Root cause (pre-fix): ArraySpan<T> / ReadOnlyArraySpan<T> (§4.15) and Span<T> /
// ReadOnlySpan<T> (§4.14) were not registered in builtinTypeArity(), so writing
// one as a type name — `method M(s: ArraySpan<int>)`, `var sp: Span<R> := …`,
// `record W(d: ArraySpan<int>)` — errored "unresolved type name". They could be
// produced only via inference from a `.AsSpan()` call.
//
// Fix: register the four concrete span value types (arity 1). They lower to the
// shared bz.arrayspan struct {ptr,i32,i32} (lowerTypeRef / isSpanLikeTypeName),
// so codegen passes them by value as parameters, stores them in locals, and
// embeds them in record fields. The owning Span<classT> keeps its disposal
// semantics across a call boundary (a `Clear()` through a Span<classT> parameter
// nulls the shared underlying buffer slots → no double-free).
//
// The IReadOnlySpan<T> INTERFACE (§4.16) was not resolvable in #959, but #970
// synthesized it as a proper builtin interface with box vtable support, so
// `: IReadOnlySpan<T>` is now a valid type annotation (see ireadonlyspan_970_test.cpp).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
bool rejectedAtCompile(const std::string& prog) {
    auto r = runAot(prog);
    return !r.ran && r.diagnostics.hasErrors();
}
const char* kR =
    "class R public static n: int := 0; public V: int; "
    "  public constructor(v: int) R.n := R.n + 1; V := v; end constructor; "
    "  destructor R.n := R.n - 1; end destructor; end R; ";
}  // namespace

// §4.15: ArraySpan<int> as a method PARAMETER (the issue's exact repro).
BEZEL_TEST(span_type_annotation_959, arrayspan_int_parameter) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private static method SumSpan(span: ArraySpan<int>): int "
        "    var t := 0; "
        "    for var x in span do t := t + x; end; "
        "    return t; "
        "  end SumSpan; "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(1); arr.Add(2); arr.Add(3); "
        "    return SumSpan(arr.AsSpan()); "  // 6
        "  end Main; end C;"), 6);
}

// §4.15: ArraySpan<int> as a LOCAL annotation.
BEZEL_TEST(span_type_annotation_959, arrayspan_int_local_annotation) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var arr := new Array<int>(); "
        "  arr.Add(2); arr.Add(3); "
        "  var sp: ArraySpan<int> := arr.AsSpan(); "
        "  return sp[0] + sp[1]; "  // 5
        "end Main; end C;"), 5);
}

// §4.14: the owning Span<R> (class element) as a method PARAMETER — reads a
// borrow of the owned element through the span.
BEZEL_TEST(span_type_annotation_959, owning_span_class_parameter) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C "
        "  private static method First(span: Span<R>): int "
        "    return span[0]!.V; "  // span[i] is R? (#1208), narrow with `!`
        "  end First; "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<R>(2); "
        "    buf[0] := new R(10); buf[1] := new R(20); "
        "    return First(buf.AsSpan()); "  // 10
        "  end Main; end C;"), 10);
}

// §4.14: ReadOnlySpan<int> as a method PARAMETER.
BEZEL_TEST(span_type_annotation_959, readonly_span_parameter) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private static method Len(span: ReadOnlySpan<int>): int "
        "    return span.Length; "
        "  end Len; "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(7); "
        "    return Len(buf.AsReadOnlySpan()); "  // 7
        "  end Main; end C;"), 7);
}

// §4.15: an ArraySpan<int> as a RECORD FIELD.
BEZEL_TEST(span_type_annotation_959, arrayspan_record_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Win(Data: ArraySpan<int>, Tag: int); "
        "class C public static method Main(): int "
        "  using var arr := new Array<int>(); "
        "  arr.Add(4); arr.Add(5); "
        "  var w := new Win(arr.AsSpan(), 9); "
        "  return w.Data[0] + w.Data[1] + w.Tag; "  // 4+5+9 == 18
        "end Main; end C;"), 18);
}

// §4.14: an owning Span<R> passed as a parameter and Clear()'d there nulls the
// SHARED underlying buffer slots — the buffer's scope-end disposal skips them.
// R.n returns to 0 (each instance disposed exactly once; no double-free).
BEZEL_TEST(span_type_annotation_959, clear_through_owning_span_parameter_no_double_free) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C "
        "  private static method Wipe(span: Span<R>) "
        "    span.Clear(); "
        "  end Wipe; "
        "  public static method Main(): int "
        "    do "
        "      using var buf := new OwningBuffer<R>(2); "
        "      buf[0] := new R(1); buf[1] := new R(2); "
        "      Wipe(buf.AsSpan()); "
        "    end; "
        "    return R.n; "  // 0
        "  end Main; end C;"), 0);
}

// §4.16 / #970: IReadOnlySpan<T> IS now resolvable as a parameter type.
// Updated from pre-#970 "rejected" guard to acceptance + value check.
// arr.Add(0) → count=1 → span.Length=1 → Len returns 1.
BEZEL_TEST(span_type_annotation_959, ireadonlyspan_interface_param_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private static method Len(span: IReadOnlySpan<int>): int "
        "    return span.Length; "
        "  end Len; "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(0); "
        "    return Len(arr.AsReadOnlySpan()); "
        "  end Main; end C;"), 1);
}
