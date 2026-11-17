// Codegen — #6406 residual (body item 4) / §28.3.4 + §29.2 + §28.9 (v0.357).
//
// THE DEFECT. A method returning `IEnumerable<T>` whose body is
//     var a: Array<int> := [1, 2, 3];
//     return a;
// leaked the whole backing `Array` on every call (300 calls under ASan/LSan:
// `Direct leak of 28800 byte(s)` + `Indirect leak of 19200 byte(s)`, both from
// `bz_array_alloc`). The sibling `var a: Array<int> := new Array<int>();`
// spelling was clean.
//
// ROOT CAUSE (MEASURED by flipping the one classification). The RETURN
// coercion `wrapAsIEnumerableForTarget` takes an OWNING wrap (slot-1 Dispose
// frees the wrapper frame AND the backing) only when
// `returnSourceIsCalleeOwnedCollection` calls the returned local a callee-owned
// producer. That predicate looked at the local's initializer and admitted a
// `NewExpr` and a `CallExpr` only. §28.3.4 *Producers* lists a collection
// literal targeting a reference type as `Owned`, and §28.3 makes `return a`
// suppress the local's own scope-end reclaim, so for a literal-initialised
// local the BORROWING wrap was used and nobody freed the backing.
//
// The same one-line gap covered every literal-initialisable collection arm of
// the wrap (Array, Stack, Queue, LinkedList, Set), generic `T`, `move a`, and
// a block-bodied lambda's `return a`; each is a row below.
//
// ORACLE. `runAotChecked` + `BEZEL_AOT_DETECT_LEAKS=strict` per row (see
// wrap_user_class_argument_frame_leak_6406_test.cpp for why the knob and the
// inverted `checkedToolchainSkipped` come FIRST). Every row loops 300x and
// reads the data (sum 300 * (1+2+3) = 1800), so neither a body that never ran
// nor a wrap that hands back garbage can pass.
#include <cstdlib>
#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

class LeakEnv {
public:
    explicit LeakEnv(const char* value) {
        const char* prev = std::getenv("BEZEL_AOT_DETECT_LEAKS");
        had_ = prev != nullptr;
        if (had_) prev_ = prev;
        ::setenv("BEZEL_AOT_DETECT_LEAKS", value, /*overwrite=*/1);
    }
    ~LeakEnv() {
        if (had_) ::setenv("BEZEL_AOT_DETECT_LEAKS", prev_.c_str(), 1);
        else      ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    LeakEnv(const LeakEnv&) = delete;
    LeakEnv& operator=(const LeakEnv&) = delete;
private:
    bool had_ = false;
    std::string prev_;
};

// `members` is the producer under test (a `static method F ...`); `use` is the
// loop body that consumes its result and adds 1+2+3 to `s`.
std::string program(const std::string& members, const std::string& use) {
    return "module TP; "
           "public class Consume "
           "  public static method Total(e: IEnumerable<int>): int "
           "    var s := 0; "
           "    for var x in e do s := s + x; end; "
           "    return s; "
           "  end Total; " +
           members +
           "end Consume; "
           "public class TPImpl "
           "  public static method Main(): int "
           "    var s := 0; "
           "    for var i in 0..<300 do " +
           use +
           "    end for; "
           "    Console.WriteLine($\"s={s}\"); "
           "    return 0; "
           "  end Main; "
           "end TPImpl;";
}

const char* kBoundUse = "      var u := Consume.F(); s := s + Consume.Total(u); ";

void expectCleanSum1800(const std::string& prog) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(prog);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1800") != std::string::npos);
}

std::string literalLocal(const std::string& ty) {
    return "  public static method F(): IEnumerable<int> "
           "    var a: " + ty + " := [1, 2, 3]; "
           "    return a; "
           "  end F; ";
}

}  // namespace

// THE ISSUE'S SHAPE (probe 6406c). §28.3.4: the literal is an Owned
// producer; §29.2: the returned IEnumerable<int> owns the backing.
BEZEL_TEST(wrap_literal_local_return_leak_6406,
           array_literal_local_returned_as_ienumerable_spec_28_3_4) {
    expectCleanSum1800(program(literalLocal("Array<int>"), kBoundUse));
}

// The unbound-temporary consumer (`for` over the call) — the frame is
// loop-owned, the backing must still be freed by the owning wrap.
BEZEL_TEST(wrap_literal_local_return_leak_6406,
           array_literal_local_returned_consumed_by_for_in_spec_29_2) {
    expectCleanSum1800(program(
        literalLocal("Array<int>"),
        "      for var x in Consume.F() do s := s + x; end; "));
}

// `return move a;` — the §28.8.1 transfer mark spelling of the same return.
BEZEL_TEST(wrap_literal_local_return_leak_6406,
           array_literal_local_move_returned_spec_28_3_7) {
    expectCleanSum1800(program(
        "  public static method F(): IEnumerable<int> "
        "    var a: Array<int> := [1, 2, 3]; "
        "    return move a; "
        "  end F; ",
        kBoundUse));
}

// Generic `T`: the element size is resolved at the instantiation (#8240) and
// the owning wrap must still free the literal's backing.
BEZEL_TEST(wrap_literal_local_return_leak_6406,
           generic_array_literal_local_returned_spec_28_3_4) {
    expectCleanSum1800(program(
        "  public static method F<T>(x: T): IEnumerable<T> "
        "    var a: Array<T> := [x, x, x]; "
        "    return a; "
        "  end F; ",
        "      var u := Consume.F<int>(2); s := s + Consume.Total(u); "));
}

// A block-bodied lambda's `return a;` (the §27.3 lambda return slot).
BEZEL_TEST(wrap_literal_local_return_leak_6406,
           lambda_block_array_literal_local_returned_spec_28_3_4) {
    expectCleanSum1800(program(
        "  public static method F(): IEnumerable<int> "
        "    var f := lambda(): IEnumerable<int> => do "
        "      var a: Array<int> := [1, 2, 3]; "
        "      return a; "
        "    end; "
        "    return f(); "
        "  end F; ",
        kBoundUse));
}

// The other literal-initialisable collection arms of the wrap.
BEZEL_TEST(wrap_literal_local_return_leak_6406,
           stack_literal_local_returned_as_ienumerable_spec_28_3_4) {
    expectCleanSum1800(program(literalLocal("Stack<int>"), kBoundUse));
}

BEZEL_TEST(wrap_literal_local_return_leak_6406,
           queue_literal_local_returned_as_ienumerable_spec_28_3_4) {
    expectCleanSum1800(program(literalLocal("Queue<int>"), kBoundUse));
}

BEZEL_TEST(wrap_literal_local_return_leak_6406,
           linkedlist_literal_local_returned_as_ienumerable_spec_28_3_4) {
    expectCleanSum1800(program(literalLocal("LinkedList<int>"), kBoundUse));
}

BEZEL_TEST(wrap_literal_local_return_leak_6406,
           set_literal_local_returned_as_ienumerable_spec_28_3_4) {
    expectCleanSum1800(program(literalLocal("Set<int>"), kBoundUse));
}
