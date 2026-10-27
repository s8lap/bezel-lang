// fix #4207 (stdlib §5 / §12.9, v0.298): terminal LINQ calls silently degrade
// to zero on explicit IOrderedEnumerable<T> named receiver.
// Root cause: sema had no isOrderedEnumerableLinqRecv arm, so calls like
// .First()/.Count()/.Any() on a named IOrderedEnumerable<int> receiver were
// left untyped; codegen had no matching lowerToLazyEnumerator dispatch arm either,
// so the call fell through to the #2491 zero/null backstop at runtime.
// Fix: add isOrderedEnumerableLinqRecv to ExprTypecheck_Calls.cpp (element at
// genericArgs[0], same as lazy sentinel) and add IOrderedEnumerable to
// lowerToLazyEnumerator/tryLowerLazyLinqCall in LlvmBackend_Calls3.cpp.
//
// spec references: stdlib §5 (IOrderedEnumerable<T>: IEnumerable<T>)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// fix #4207: explicitly-annotated IOrderedEnumerable<int> receiver + .First()
// FAILS-WITHOUT: exit code 1 (returns 0, wrong value — silent degrade via #2491)
// PASSES-WITH: exit code 0 (returns 1, correct sorted-first value)
BEZEL_TEST(iorderedenumerable_terminal_linq_named_receiver_4207,
           issue_explicit_annotation_first) {
    auto r = runAot(
        "module M; "
        "public static class Program "
        "  public static method GetSorted(xs: Array<int>): IOrderedEnumerable<int> => "
        "      xs.OrderBy(lambda(x) => x); "
        "  public static method Test(): int "
        "    var xs: Array<int> := [3, 1, 2]; "
        "    var sorted: IOrderedEnumerable<int> := GetSorted(xs); "
        "    return sorted.First(); "
        "  end Test; "
        "  public static method Main(): int "
        "    var r := Test(); "
        "    Console.WriteLine(\"r\"); "
        "    if r <> 1 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    if (!r.ran || r.exitCode != 0) {
        std::cout << "    #4207 explicit annotation + .First(): (ran="
                  << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::cout << "      DIAG: " << d.message << "\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// fix #4207: Count on explicit IOrderedEnumerable<int>
// FAILS-WITHOUT: exit code 1 (returns 0, wrong value)
// PASSES-WITH: exit code 0 (returns 3)
BEZEL_TEST(iorderedenumerable_terminal_linq_named_receiver_4207,
           explicit_annotation_count) {
    auto r = runAot(
        "module M; "
        "public static class Program "
        "  public static method Test(): int "
        "    var xs: Array<int> := [3, 1, 2]; "
        "    var sorted: IOrderedEnumerable<int> := xs.OrderBy(lambda(x) => x); "
        "    return sorted.Count(); "
        "  end Test; "
        "  public static method Main(): int "
        "    var r := Test(); "
        "    Console.WriteLine(\"c\"); "
        "    if r <> 3 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    if (!r.ran || r.exitCode != 0) {
        std::cout << "    #4207 explicit annotation + .Count(): (ran="
                  << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::cout << "      DIAG: " << d.message << "\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// fix #4207: Any on explicit IOrderedEnumerable<int>
// FAILS-WITHOUT: exit code 1 (returns 0/false, wrong value)
// PASSES-WITH: exit code 0 (returns true -> 0)
BEZEL_TEST(iorderedenumerable_terminal_linq_named_receiver_4207,
           explicit_annotation_any) {
    auto r = runAot(
        "module M; "
        "public static class Program "
        "  public static method Test(): bool "
        "    var xs: Array<int> := [3, 1, 2]; "
        "    var sorted: IOrderedEnumerable<int> := xs.OrderBy(lambda(x) => x); "
        "    return sorted.Any(); "
        "  end Test; "
        "  public static method Main(): int "
        "    var r := Test(); "
        "    Console.WriteLine(\"a\"); "
        "    if not r then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    if (!r.ran || r.exitCode != 0) {
        std::cout << "    #4207 explicit annotation + .Any(): (ran="
                  << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::cout << "      DIAG: " << d.message << "\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// fix #4207: helper function with explicit IOrderedEnumerable<int> return type + .First()
// PASSES-WITH: exit code 0 (returns 1, the min after sort)
BEZEL_TEST(iorderedenumerable_terminal_linq_named_receiver_4207,
           helper_explicit_return_first) {
    auto r = runAot(
        "module M; "
        "public static class Program "
        "  public static method GetSorted(xs: Array<int>): IOrderedEnumerable<int> => "
        "      xs.OrderBy(lambda(x) => x); "
        "  public static method Test(): int "
        "    var xs: Array<int> := [5, 2, 8, 1]; "
        "    return GetSorted(xs).First(); "
        "  end Test; "
        "  public static method Main(): int "
        "    var r := Test(); "
        "    Console.WriteLine(\"h\"); "
        "    if r <> 1 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    if (!r.ran || r.exitCode != 0) {
        std::cout << "    #4207 helper explicit return + .First(): (ran="
                  << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::cout << "      DIAG: " << d.message << "\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Control: inferred-var form (always worked) vs explicit annotation (was broken)
BEZEL_TEST(iorderedenumerable_terminal_linq_named_receiver_4207,
           inferred_vs_explicit) {
    auto r = runAot(
        "module M; "
        "public static class Program "
        "  public static method TestExplicit(): int "
        "    var xs: Array<int> := [9, 1, 7]; "
        "    var s: IOrderedEnumerable<int> := xs.OrderBy(lambda(x) => x); "
        "    return s.First(); "
        "  end TestExplicit; "
        "  public static method TestInferred(): int "
        "    var xs: Array<int> := [9, 1, 7]; "
        "    var s := xs.OrderBy(lambda(x) => x); "
        "    return s.First(); "
        "  end TestInferred; "
        "  public static method Main(): int "
        "    var exp := TestExplicit(); "
        "    var inf := TestInferred(); "
        "    Console.WriteLine(\"x\"); "
        "    if exp <> 1 then return 1; end; "
        "    if inf <> 1 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    if (!r.ran || r.exitCode != 0) {
        std::cout << "    #4207 inferred vs explicit: (ran="
                  << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::cout << "      DIAG: " << d.message << "\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
