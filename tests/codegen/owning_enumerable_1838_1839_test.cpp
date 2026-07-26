// Codegen regressions #1838/#1839 — §29 direct IOwningEnumerable sources.
#include <string>
#include "support/aot_support.hpp"
#include "test_harness.hpp"
using bezel::test::runAotChecked;
namespace {
int run(const std::string& s) { auto r=runAotChecked(s); return r.ran ? r.exitCode : -999; }
}

// §29: a local IOwningEnumerable<T> is a vtable-backed source, not a raw array.
BEZEL_TEST(codegen_owning_enumerable_1838, local_source_enumerates) {
  BEZEL_EXPECT_EQ(run("module M; class C public static method Main(): int "
    "var src: IOwningEnumerable<int> := new OwningArray<int>(); "
    "src.Add(1); src.Add(2); var sum:=0; "
    "for var x in src do sum:=sum+x; end; return sum; end Main; end C;"), 3);
}

// §29: widening an Array<T> to an owned enumerable must retain its wrapper.
BEZEL_TEST(codegen_owning_enumerable_1839, array_widening_disposes_cleanly) {
  BEZEL_EXPECT_EQ(run("module M; class C public static method Main(): int "
    "var arr: Array<int> := [1,2,3]; "
    "using var e: IOwningEnumerable<int> := arr; return 0; end Main; end C;"), 0);
}
