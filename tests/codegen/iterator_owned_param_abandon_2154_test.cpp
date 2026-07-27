// Regression tests for #2154 / #2217. Per §29.8 v0.226, an iterator
// state-machine disposes every `owned` parameter that has not yet moved out when
// the iterator is torn down. For the linear owning-iterator shape below, the
// enumerator clone owns the captures and keys cleanup to the reached yield state.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

std::string program(const std::string& drive) {
    return
        "module M; "
        "class R "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "  destructor Console.WriteLine(\"dispose \" + Id.ToString()); end destructor; "
        "end R; "
        "class C "
        "  public static method Gen(owned a: R, owned b: R): IOwningEnumerable<R> "
        "    yield return a; "
        "    yield return b; "
        "  end Gen; "
        "  public static method Main(): int "
        "    do "
        "      using var source: IOwningEnumerable<R> := Gen(new R(1), new R(2)); "
        "      using var en: IOwningEnumerator<R> := source.GetOwningEnumerator(); " +
        drive +
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;";
}

void expectCleanAndDisposedOnce(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "dispose 1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "dispose 2"), 1);
}

}  // namespace

BEZEL_TEST(iterator_owned_param_abandon_2154,
           break_after_first_transfer_disposes_remaining_spec_29_8) {
    expectCleanAndDisposedOnce(program(
        "      while en.MoveNext() do "
        "        using var u := en.TakeCurrent(); "
        "        break; "
        "      end; "));
}

BEZEL_TEST(iterator_owned_param_abandon_2154,
           never_started_disposes_both_captures_spec_29_8) {
    expectCleanAndDisposedOnce(program(""));
}

BEZEL_TEST(iterator_owned_param_abandon_2154,
           full_drain_does_not_redispose_transfers_spec_29_8) {
    expectCleanAndDisposedOnce(program(
        "      while en.MoveNext() do "
        "        using var u := en.TakeCurrent(); "
        "      end; "));
}
