// fix #5466 (§3.6 rule 2 / §14.2, v0.315): builtin collection types
// (OwningArray, OwningSet, OwningDictionary, …) were incorrectly rejected
// as "bare type parameter" when calling members like .Add() / .Count.
//
// Root cause: ExprTypecheck_Calls.cpp's #5449 check fired for any
// Named TypeRef with declaration==null, including builtinClassSym-minted
// types. The fix adds isInScopeTypeParam(paramName) to the condition,
// ensuring the check only fires for genuine method/class generic
// parameters (which have a declared scope), not for builtin types whose
// TypeRef merely has null declaration.
//
// This test verifies that member access on OwningArray/OwningSet/
// OwningDictionary is accepted, and that the #5449 behaviour (genuine
// bare type parameter with non-constraint member is rejected) is preserved.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Helper: verify a program compiles and runs with expected exit code.
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must
// be distinguished by asserting on the diagnostic TEXT.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// fix #5466: OwningSet<int>.Add() / .Count must be accepted.
BEZEL_TEST(owning_collection_member_access_5466, owning_set_member_access_accepted) {
    // OwningSet<int> requires class element (stdlib §4.2 / §14.4, #1327).
    // Use Box as the element type.
    const std::string program =
        "module M; import System.Collections.Generic; "
        "class Box public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  destructor end destructor; end Box; "
        "class C public static method Main(): int "
        "  using var s: OwningSet<Box> := new OwningSet<Box>(); "
        "  s.Add(new Box(1)); "
        "  s.Add(new Box(2)); "
        "  if s.Count <> 2 then return 90; end; "
        "  return 42; "
        "end Main; end C;";
    auto r = runAotChecked(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// fix #5466: OwningArray<int>.Add() / .Count must be accepted.
BEZEL_TEST(owning_collection_member_access_5466, owning_array_member_access_accepted) {
    const std::string program =
        "module M; import System.Collections.Generic; "
        "class Box public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  destructor end destructor; end Box; "
        "class C public static method Main(): int "
        "  using var a: OwningArray<Box> := new OwningArray<Box>(); "
        "  a.Add(new Box(1)); "
        "  a.Add(new Box(2)); "
        "  if a.Count <> 2 then return 90; end; "
        "  return 42; "
        "end Main; end C;";
    auto r = runAotChecked(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// fix #5466: OwningDictionary<string, int>.Add() / .Count must be accepted.
BEZEL_TEST(owning_collection_member_access_5466, owning_dictionary_member_access_accepted) {
    const std::string program =
        "module M; import System.Collections.Generic; "
        "class Box public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "  destructor end destructor; end Box; "
        "class C public static method Main(): int "
        "  using var d: OwningDictionary<string, Box> := new OwningDictionary<string, Box>(); "
        "  d.Add(\"a\", new Box(1)); "
        "  d.Add(\"b\", new Box(2)); "
        "  if d.Count <> 2 then return 90; end; "
        "  return 42; "
        "end Main; end C;";
    auto r = runAotChecked(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// fix #5466: other builtin collection types should also work.
// SortedSet<int>.Add() / .Count must be accepted.
BEZEL_TEST(owning_collection_member_access_5466, sorted_set_member_access_accepted) {
    const std::string program =
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var s: SortedSet<int> := new SortedSet<int>(); "
        "  s.Add(1); "
        "  s.Add(2); "
        "  if s.Count <> 2 then return 90; end; "
        "  return 42; "
        "end Main; end C;";
    auto r = runAotChecked(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// fix #5449 preservation: a genuine bare type parameter with a
// non-constraint member must STILL be rejected. This row ensures the
// fix for #5466 did not over-widen and disable the #5449 check.
BEZEL_TEST(owning_collection_member_access_5466,
           bare_type_param_non_constraint_member_still_rejected) {
    // T is an unconstrained type parameter — it has no members except
    // the universal Object members. .Bogus is not one of them and
    // must be rejected at the declaration site.
    const std::string program =
        "module M; "
        "record Pt(X: int, Y: int); "
        "class Container<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method GX(): int return _val.Bogus; end GX; "  // T has no .Bogus
        "  destructor end destructor; "
        "end Container; "
        "class C public static method Main(): int "
        "  using var c := new Container<Pt>(new Pt(9, 5)); "
        "  return c.GX(); "
        "end Main; end C;";
    // Must be rejected at compile time with the bare type parameter diagnostic.
    BEZEL_EXPECT_TRUE(hasError(program, "no member `Bogus` on bare type parameter `T`"));
}
