// Codegen — a for-loop inside a SUSPENDING async iterator must spill its
// implicit locals (index, loop variable, enumerator) into the state-machine
// frame (§29.3 / §30.7, #747 R42).
//
// The for-loop's index/var/enumerator are plain stack allocas; their live range
// crosses the `yield return` suspend, but fixupSuspendDominance only demotes
// REGISTERS (allocas are trivially-dominating and skipped). On a cross-thread
// Resume they read fresh-stack garbage → only the first element is yielded
// (for-over-Array) or a segfault (for-over-IEnumerable, the enumerator pointer).
// The fix registers those allocas into asyncHandlerSpillSlots_ when the loop
// body suspends, so they are heap-spilled into the persistent frame.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {
namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDirAifs747() {
    return wtScratchDir("wt-aifs747");
}

// Compile to a temp native executable via the real Driver and run it (async
// iterators need the full async runtime). Returns the process exit code.
int compileAndRun(const std::string& source) {
    auto dir = wtDirAifs747();
    std::string stem = unique_stem("bezel_aifs747");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}
}  // namespace

// Finding 3: `for var x in <Array<int>>` inside an async iterator yielded ONLY
// the first element (the index alloca read garbage on resume). Now all three.
BEZEL_TEST(async_iter_frame_spill_747, for_over_array_yields_all_elements) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public async static method Gen(items: Array<int>): IAsyncEnumerable<int>
    for var x in items do
      yield return x;
    end;
  end Gen;
  public static async method Main(): Task<int>
    using var a := new Array<int>();
    a.Add(10); a.Add(20); a.Add(30);
    var sum := 0;
    await for var v in Gen(a) do sum := sum + v; end;
    return sum;
  end Main;
end C;
)"), 60);
}

// Finding 2: `for var x in <IEnumerable<int>>` inside an async iterator
// SEGFAULTED (the enumerator alloca read garbage on resume). Now sums all.
BEZEL_TEST(async_iter_frame_spill_747, for_over_ienumerable_no_segfault) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public async static method Gen(items: IEnumerable<int>): IAsyncEnumerable<int>
    for var x in items do
      yield return x;
    end;
  end Gen;
  public static async method Main(): Task<int>
    using var a := new Array<int>();
    a.Add(10); a.Add(20); a.Add(30);
    var sum := 0;
    await for var v in Gen(a) do sum := sum + v; end;
    return sum;
  end Main;
end C;
)"), 60);
}

// The LOOP VARIABLE must survive the suspend too: the body reads `x` AFTER a
// yield. For {1,2}: yields 1,10,2,20 → sum 33.
BEZEL_TEST(async_iter_frame_spill_747, loop_var_survives_suspend) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public async static method Gen(items: Array<int>): IAsyncEnumerable<int>
    for var x in items do
      yield return x;
      yield return x * 10;
    end;
  end Gen;
  public static async method Main(): Task<int>
    using var a := new Array<int>();
    a.Add(1); a.Add(2);
    var sum := 0;
    await for var v in Gen(a) do sum := sum + v; end;
    return sum;
  end Main;
end C;
)"), 33);
}

// CONTROL (must not regress): a range-for inside an async iterator already
// worked (its single slot is frame-backed); the new spill must not perturb it.
BEZEL_TEST(async_iter_frame_spill_747, range_for_in_async_iter_unregressed) {
    BEZEL_EXPECT_EQ(compileAndRun(R"(
module M;
class C
  public async static method Gen(): IAsyncEnumerable<int>
    for var i in 1..3 do
      yield return i;
    end;
  end Gen;
  public static async method Main(): Task<int>
    var sum := 0;
    await for var v in Gen() do sum := sum + v; end;
    return sum;
  end Main;
end C;
)"), 6);
}
