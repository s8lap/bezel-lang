// Spec-derived tests for §19.3/§12.7/§2.8.3 rule 4 (#2001, resolves #1971, v0.197): the
// #1946 predicate `typeArgRuntimeReified` (src/sema/ExprTypecheck.cpp) gates the
// DEAD-ARM admission of a typed binding `var x: C<A>` against a generic INTERFACE
// scrutinee. It formerly treated EVERY inline-array type argument as erased (not
// runtime-distinguishable), so a live arm whose differing argument was an inline
// array (`C<[3]int>`, `C<Box<[3]int>>`) was REJECTED — a conservative over-reject
// (#1971).
//
// That over-reject was soundness-NECESSARY only while codegen's descriptor key
// (appendDescKey) omitted the inline-array size N: `[3]int` and `[4]int` then
// collapsed onto ONE runtime descriptor (a memory-unsafe collision, #2001). #2001
// makes the size runtime-precise (the key encodes N, descArgsConcrete admits a
// folded-size inline array), so a FOLDED inline array whose element is itself
// reified IS now runtime-distinguishable. This test pins the resulting admission
// — and confirms the precision boundary is unchanged for a still-erased kind (a
// function-typed argument, #1939, stays REJECTED).

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff an ERROR diagnostic carries the #1946 ERASED-ambiguous reason — a live
// arm whose differing type argument is a still-erased #1939 kind, so the runtime
// test degrades and a sibling would mis-bind ("not runtime-distinguishable").
bool hasErasedAmbiguousError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not runtime-distinguishable") != std::string::npos)
            return true;
    }
    return false;
}

std::string wrap(const std::string& decls, const std::string& body) {
    return "module M;\n" + decls + "class Program\n" + body + "end Program;\n";
}

}  // namespace

// ---- #1971 admission: a FOLDED inline-array arg is now reified — ACCEPTED ------

// (a) DIRECT inline-array argument: `C<T>: IMarker<T>, IMarker<int>` at scrutinee
// `IMarker<int>`, arm `var x: C<[3]int>`. `C<[3]int>` conforms to `IMarker<int>`
// via the GROUND clause, so a `C<[3]int>` value legitimately flows through the
// scrutinee; post-#2001 `is C<[3]int>` is precise (the descriptor key encodes N),
// so only a genuine `C<[3]int>` binds. LIVE and sound; ACCEPTED (was over-rejected
// "not runtime-distinguishable").
BEZEL_TEST(case_type_test_iface_bind_inline_array_2001,
           direct_inline_array_arg_admitted) {
    auto r = parseSemaAll(wrap(
        "interface IMarker<T>\nend IMarker;\n"
        "sealed class C<T> : IMarker<T>, IMarker<int>\nend C;\n",
        "  public method M(m: IMarker<int>): int\n"
        "    var n := (case m of\n"
        "      var x: C<[3]int> => 1,\n"
        "      _ => 0\n"
        "    );\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasErasedAmbiguousError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (b) NESTED inline-array argument (the exact #1971 shape): arm
// `var x: C<Box<[3]int>>`. `Box<[3]int>` is a concrete Named type whose own
// argument `[3]int` is a folded inline array, so it reifies recursively; the
// descriptor distinguishes it from `C<Box<[4]int>>`. LIVE and sound; ACCEPTED.
BEZEL_TEST(case_type_test_iface_bind_inline_array_2001,
           nested_inline_array_arg_admitted) {
    auto r = parseSemaAll(wrap(
        "interface IMarker<T>\nend IMarker;\n"
        "class Box<U>\nend Box;\n"
        "sealed class C<T> : IMarker<T>, IMarker<int>\nend C;\n",
        "  public method M(m: IMarker<int>): int\n"
        "    var n := (case m of\n"
        "      var x: C<Box<[3]int>> => 1,\n"
        "      _ => 0\n"
        "    );\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasErasedAmbiguousError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- Precision boundary unchanged: a still-erased kind stays REJECTED ----------

// A FUNCTION-typed argument (#1939) is NOT runtime-reified — the descriptor key
// collapses `Kind::Function` to "F" at any depth — so `is C<(int)->int>` degrades
// and a `C<(string)->string>` sibling would mis-bind. The uniqueness rejection
// must STAND (the #2001 admission is scoped to folded inline arrays, NOT functions).
BEZEL_TEST(case_type_test_iface_bind_inline_array_2001,
           function_arg_still_erased_rejected) {
    auto r = parseSemaAll(wrap(
        "interface IMarker<T>\nend IMarker;\n"
        "sealed class C<T> : IMarker<T>, IMarker<int>\nend C;\n",
        "  public method M(m: IMarker<int>): int\n"
        "    var n := (case m of\n"
        "      var x: C<(int) -> int> => 1,\n"
        "      _ => 0\n"
        "    );\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasErasedAmbiguousError(r));
}
