// fix #4360 (§28.3, v0.298): an instance method whose every return path is a
// fresh owned producer AND the return is not bare `this` (which has the receiver
// as its structural source and is always a borrow) is an instance factory — the
// caller must treat the call as Owned. Without this, `method Make(): C => new C(); end`
// classified Borrowed (the non-static fallthrough default) and the fresh allocation
// leaked silently at every call site (24 B/call + payload).
//
// Tests 1-4: AOT runtime tests verifying clean execution.
//
// UPDATE (§28.3 announcement table row 4, v0.307): test 5 originally asserted
// that `method Make(): C return new C(); end` — the unmarked shape #4360
// fixed the CALLER-side leak for — still COMPILES, merely producing a
// runtime auto-drop warning once correctly classified Owned. A later fix
// closed the matching DECLARATION-site half of row 4 this file's own #4360
// comment already quotes the spec for ("an instance method that produces a
// fresh value must declare `owned` — §28.6 rejects a freshly-produced owned
// value returned through a non-`owned` return"): that shape is now a
// COMPILE ERROR at its own declaration, not merely a warned-about leak risk.
// #4360's fix is not wrong — every accepted call site it re-classifies
// (an EXPLICITLY `owned`-returning factory, tests 1-2; a receiver-sourced
// borrow, tests 3-4) remains exactly as it was — but the ONE shape test 5
// exercised (an UNMARKED instance factory) is now structurally unreachable:
// the program that would trigger the leak this file was written to catch no
// longer compiles at all, which is a strictly stronger guarantee than a
// runtime warning. Test 5 now asserts the REJECTION instead of the warning;
// the anti-vacuity `this`-return control is unchanged (a receiver-sourced
// borrow was and remains legal). // per §28.3, v0.307

#include <string>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
#include <fstream>
#include <sys/wait.h>

#include "support/aot_support.hpp"  // runAotChecked
#include "support/unique_stem.hpp"  // bezel_test::unique_stem
#include "test_harness.hpp"
#include "driver/Driver.hpp"

using bezel::test::runAotChecked;

// Compile-only helper: compiles `src`, captures driver stderr into `captured`.
// Returns 0 on success, -1 on compile failure.
static int compileAndCapture(const std::string& src, std::string* captured) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz4360_" + bezel_test::unique_stem("");
    std::string srcPath = (dir / (stem + ".bzl")).string();
    std::string exePath = (dir / stem).string();
    std::string errPath = (dir / (stem + ".err")).string();

    {
        std::ofstream out(srcPath);
        out << src;
    }

    if (captured) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        bezel::Driver driver;
        bool ok = driver.compileFile(srcPath, exePath, /*deps=*/{}).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(errPath);
        captured->assign(std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>());
        return ok ? 0 : -1;
    } else {
        bezel::Driver driver;
        bool ok = driver.compileFile(srcPath, exePath, /*deps=*/{}).success;
        return ok ? 0 : -1;
    }
}

// Control: the `: owned` annotation makes the call Owned by declaration;
// pre- and post-fix both clean.
BEZEL_TEST(instance_method_owned_return_annotation_gap_4360,
            control_owned_annotation_clean) {
    std::string src =
        "module M; "
        "class C "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end C; "
        "class D "
        "  public method Make(): owned C "
        "    return new C(); "
        "  end Make; "
        "end D; "
        "class Program "
        "  public static method Run(): int "
        "    var d := new D(); "
        "    var total := 0; "
        "    for var i in 1..500 do "
        "      var c := d.Make(); "
        "      total := total + 1; "
        "    end; "
        "    return total; "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := Run(); "
        "    Console.WriteLine(r.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto res = runAotChecked(src);
    BEZEL_EXPECT_TRUE(res.ran);
    BEZEL_EXPECT_EQ(res.exitCode, 0);
}

// Control: a static factory (already handled by staticFactoryReturnsFreshOwned)
// stays clean — guards against regression.
BEZEL_TEST(instance_method_owned_return_annotation_gap_4360,
            control_static_factory_clean) {
    std::string src =
        "module M; "
        "class C "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end C; "
        "class D "
        "  public static method Make(): owned C "
        "    return new C(); "
        "  end Make; "
        "end D; "
        "class Program "
        "  public static method Run(): int "
        "    var total := 0; "
        "    for var i in 1..500 do "
        "      var c := D.Make(); "
        "      total := total + 1; "
        "    end; "
        "    return total; "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := Run(); "
        "    Console.WriteLine(r.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto res = runAotChecked(src);
    BEZEL_EXPECT_TRUE(res.ran);
    BEZEL_EXPECT_EQ(res.exitCode, 0);
}

// Instance method returning bare `this` — this is a borrow (structural source:
// the receiver), not a fresh allocation. It must compile and run cleanly.
// No auto-drop warning is expected because returning `this` is a borrow.
BEZEL_TEST(instance_method_owned_return_annotation_gap_4360,
            instance_method_returning_bare_this_clean) {
    std::string src =
        "module M; "
        "class C "
        "  public constructor() end constructor; "
        "  public method GetSelf(): C "
        "    return this; "
        "  end GetSelf; "
        "end C; "
        "class Program "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    var s := c.GetSelf(); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto res = runAotChecked(src);
    BEZEL_EXPECT_TRUE(res.ran);
    BEZEL_EXPECT_EQ(res.exitCode, 0);
}

// Instance method returning an owned field read — reading an owned field is a
// move of the owned value, transferring ownership to the caller. The method IS
// an instance factory and the return is Owned.
BEZEL_TEST(instance_method_owned_return_annotation_gap_4360,
            instance_method_returning_owned_field_clean) {
    std::string src =
        "module M; "
        "class Inner "
        "  public v: int; "
        "  public constructor(v: int) this.v := v; end constructor; "
        "end Inner; "
        "class Outer "
        "  public owned inner: Inner; "
        "  public constructor() this.inner := new Inner(42); end constructor; "
        "  public method GetInner(): Inner "
        "    return this.inner; "
        "  end GetInner; "
        "end Outer; "
        "class Program "
        "  public static method Main(): int "
        "    var o := new Outer(); "
        "    var i := o.GetInner(); "
        "    return i.v; "
        "  end Main; "
        "end Program;";
    auto res = runAotChecked(src);
    BEZEL_EXPECT_TRUE(res.ran);
    BEZEL_EXPECT_EQ(res.exitCode, 42);
}

// 5th test — mechanical compile-only assertion that the fix changed producerState
// classification. UPDATED (§28.3 announcement table row 4, v0.307): the
// unmarked shape below — `method Make(): C return new C(); end` — is now
// REJECTED at its own declaration (a later, stricter fix than #4360's own;
// see the file header). It can therefore no longer demonstrate a Borrowed-
// vs-Owned CALLER-side classification difference at all — the call site is
// unreachable because the callee never compiles. This test now asserts the
// rejection directly, citing the row-4 diagnostic.
//
// Anti-vacuity twin (bare `this` return) compiles WITHOUT auto-drop warning,
// proving #4360's classification fix still stands for the shapes it must
// (a receiver-sourced borrow is not, and was never, an instance factory).
BEZEL_TEST(instance_method_owned_return_annotation_gap_4360,
            unmarked_instance_factory_rejected_at_declaration) {
    // The program under test: instance method returning `new C()` without `: owned`.
    std::string srcFactory =
        "module M; "
        "class C "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end C; "
        "class D "
        "  public method Make(): C "
        "    return new C(); "
        "  end Make; "
        "end D; "
        "class Program "
        "  public static method Main(): int "
        "    var d := new D(); "
        "    var c := d.Make(); "
        "    return 0; "
        "  end Main; "
        "end Program;";

    std::string captured;
    int ret = compileAndCapture(srcFactory, &captured);

    // §28.3 announcement table row 4 (v0.307): an unmarked instance-method
    // return admits only a receiver-sourced borrow; `Make()`'s fresh `new
    // C()` has no announced transfer and is rejected at its own declaration.
    BEZEL_EXPECT_EQ(ret, -1);
    bool hasRow4Rejection =
        captured.find("announcement table row 4") != std::string::npos;
    BEZEL_EXPECT_TRUE(hasRow4Rejection);

    // Anti-vacuity: bare `this` return must NOT produce an auto-drop warning
    // (it is a borrow, not an owned factory).
    std::string srcThis =
        "module M; "
        "class C "
        "  public constructor() end constructor; "
        "  public method GetSelf(): C "
        "    return this; "
        "  end GetSelf; "
        "end C; "
        "class Program "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    var s := c.GetSelf(); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    std::string capturedThis;
    int retThis = compileAndCapture(srcThis, &capturedThis);
    BEZEL_EXPECT_EQ(retThis, 0);
    bool thisHasAutoDrop = capturedThis.find("auto-drop") != std::string::npos;
    BEZEL_EXPECT_FALSE(thisHasAutoDrop);
}
