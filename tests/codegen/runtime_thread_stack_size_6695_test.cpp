// Codegen/runtime — §30.14 (Domain.Spawn), §30.13 (async continuations on the
// scheduler pool), v0.339 — #6695: every thread the runtime creates gets an
// explicit BZ_THREAD_STACK_SIZE (8 MiB) stack.
//
// Pre-fix the scheduler pool and the timer thread were bare std::threads, so
// a worker's stack was the platform default: musl's fixed 128 KiB (the #6654
// spike segfaulted a 20000-deep recursion on a Domain.Spawn worker), and on
// glibc whatever RLIMIT_STACK said at program start. The rows below make the
// glibc dependence observable: they lower the soft RLIMIT_STACK to 512 KiB
// around the runAot call (the program inherits it through the harness's
// fork+exec), so a worker that takes the platform default gets a 512 KiB
// stack and the recursion segfaults (MEASURED on e5887ed8: rc 139), while a
// worker created with the explicit 8 MiB stack runs it. The program's own
// main thread is unaffected in practice: Main is a few frames deep, and the
// recursion runs only on pool workers.
//
// The default-limit row checks the deeper recursion still runs with the
// ambient limit, so the explicit size never shrinks a worker below what the
// platform would have given (8 MiB = glibc's usual default).

#include <string>

#include <sys/resource.h>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Lowers the SOFT stack limit of this process for one scope and restores it.
// The tests run one at a time in a shard process, so no sibling observes it.
class ScopedSoftStackLimit {
public:
    explicit ScopedSoftStackLimit(rlim_t bytes) {
        ok_ = ::getrlimit(RLIMIT_STACK, &saved_) == 0;
        if (!ok_) return;
        struct rlimit lowered = saved_;
        lowered.rlim_cur = bytes;
        ok_ = ::setrlimit(RLIMIT_STACK, &lowered) == 0;
    }
    ~ScopedSoftStackLimit() {
        if (ok_) ::setrlimit(RLIMIT_STACK, &saved_);
    }
    bool ok() const { return ok_; }

private:
    struct rlimit saved_{};
    bool ok_ = false;
};

constexpr rlim_t kLowSoftStack = 512 * 1024;

// The recursion is a real (non-tail) call chain: `1 + Deep(n - 1)`. 20000
// frames overflow 512 KiB (MEASURED) and fit comfortably in 8 MiB, also under
// ASan's larger frames (MEASURED: 100000 deep runs under --sanitize=address).
std::string spawnDeep(int depth) {
    return "module DeepWorker;\n"
           "\n"
           "public class Program\n"
           "  private static method Deep(n: int): int\n"
           "    if n = 0 then\n"
           "      return 0;\n"
           "    end;\n"
           "    return 1 + Deep(n - 1);\n"
           "  end Deep;\n"
           "\n"
           "  public static async method Main(): Task\n"
           "    using var t := Domain.Spawn(Deep, " + std::to_string(depth) + ");\n"
           "    var r := await t;\n"
           "    Console.WriteLine($\"{r}\");\n"
           "  end Main;\n"
           "end Program;\n";
}

}  // namespace

// §30.14: the issue's repro — the spawned entry recurses 20000 deep on a pool
// worker. Pre-fix, with a 512 KiB soft RLIMIT_STACK, the worker inherited a
// 512 KiB stack and the program died of SIGSEGV (ran=false).
BEZEL_TEST(runtime_thread_stack_size_6695,
           domain_spawn_worker_recursion_ignores_low_rlimit_stack) {
    ScopedSoftStackLimit limit(kLowSoftStack);
    BEZEL_EXPECT_TRUE(limit.ok());
    const auto r = runAot(spawnDeep(20000));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("20000\n"));
}

// §30.13: an async method's continuation after an await runs on a pool
// worker too, so the same recursion there needs the same stack.
BEZEL_TEST(runtime_thread_stack_size_6695,
           async_continuation_recursion_ignores_low_rlimit_stack) {
    const std::string src =
        "module DeepCont;\n"
        "\n"
        "public class Program\n"
        "  private static method Deep(n: int): int\n"
        "    if n = 0 then\n"
        "      return 0;\n"
        "    end;\n"
        "    return 1 + Deep(n - 1);\n"
        "  end Deep;\n"
        "\n"
        "  public static async method Main(): Task\n"
        "    await Task.Delay(1);\n"
        "    Console.WriteLine($\"{Deep(20000)}\");\n"
        "  end Main;\n"
        "end Program;\n";
    ScopedSoftStackLimit limit(kLowSoftStack);
    BEZEL_EXPECT_TRUE(limit.ok());
    const auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("20000\n"));
}

// With the ambient limit, a recursion five times deeper still runs on a
// worker: the explicit stack is not smaller than the glibc default it
// replaces.
BEZEL_TEST(runtime_thread_stack_size_6695,
           domain_spawn_worker_deep_recursion_runs_with_ambient_limit) {
    const auto r = runAot(spawnDeep(100000));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100000\n"));
}
