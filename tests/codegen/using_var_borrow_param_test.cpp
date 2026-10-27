// §28.10's "`using var` initialized from a borrow" — the BORROW-PIN shape whose
// source is a PLAIN PARAMETER (fix #7387).
//
// §28.10 (v0.345): "When the right-hand side of a `using var` declaration is
// a borrow rather than an owned value (... an unmarked read of a **borrow-state**
// named binding (a plain parameter, a borrowing alias or loop variable, a
// captured owned binding inside a closure) ...), the `using var` does **not**
// assume a disposal obligation (derives: K2 — no obligation can be assumed
// from a borrow): at scope end, no `Dispose()` is called, because the value is
// not owned by this scope."
//
// §28.3's K1 makes this an OWNERSHIP CLASSIFICATION question: a plain parameter
// is a BORROW of the caller's value, so the CALLER keeps the disposal
// obligation. The `using var` binding in the callee disposes nothing.
//
// The defect (#7387): the ownership pass's `usingVarSourceIsBorrow` flag does
// not cover the plain-parameter shape (its bare-identifier arm recognizes a
// tracked local and an `owned` parameter), so every scope-end disposal arm in
// `registerUsingDisposal` registered a disposal for a BORROW — the callee freed
// the caller's object and the caller's next read was a heap-use-after-free.
//
// THE ORACLE. `runAotChecked` instruments the GENERATED code with
// AddressSanitizer/LeakSanitizer, so a use-after-free flips `ran` to false and
// a leak flips it too. The printed output is asserted as well, and it is the
// output that says WHO disposed: the callee must print no `DTOR`, so the
// program's single `DTOR` is the CALLER's own `src.Dispose()`.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// fix #7387 (§28.10, v0.345): the INTERFACE-typed parameter.
//
// Before the fix: the callee disposed the borrowed parameter (`DTOR` printed
// TWICE — once by the callee, once by the caller) and the caller's read of the
// freed object was an ASan heap-use-after-free:
//   "==1600653==ERROR: AddressSanitizer: heap-use-after-free ... READ of size 8
//    freed by thread T0 here: #1 bz$Probe.Program$Take"
// After the fix: the callee disposes nothing, the caller's `src.Dispose()`
// prints the single `DTOR`, and the program runs clean.
BEZEL_TEST(using_var_borrow_param, interface_param_borrow_is_not_disposed_spec_28_10_7387) {
    bezel::test::JitResult r = runAotChecked(R"(
module M;
public class Res
  public constructor() end constructor;
  destructor Console.WriteLine("DTOR"); end destructor;
end Res;
public class Program
  public static method Take(src: IAsyncDisposable)
    using var x := src;
  end Take;
  public static async method Main(): Task
    var src: IAsyncDisposable := new Res();
    Take(src);
    src.Dispose();
    Console.WriteLine("END");
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "DTOR\nEND\n");
}

// fix #7387 (§28.10, v0.345): the SAME shape with a DECLARED interface type.
// The DECLARED spelling disposes the borrow too before the fix, because
// `classOfLocal` reads the class off the declared type and never reaches its
// own bare-identifier parameter test.
BEZEL_TEST(using_var_borrow_param, declared_interface_param_borrow_is_not_disposed_spec_28_10_7387) {
    bezel::test::JitResult r = runAotChecked(R"(
module M;
public class Res
  public constructor() end constructor;
  destructor Console.WriteLine("DTOR"); end destructor;
end Res;
public class Program
  public static method Take(src: IAsyncDisposable)
    using var x: IAsyncDisposable := src;
  end Take;
  public static async method Main(): Task
    var src: IAsyncDisposable := new Res();
    Take(src);
    src.Dispose();
    Console.WriteLine("END");
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "DTOR\nEND\n");
}

// fix #7387 (§28.10, v0.345): the CLASS-typed parameter with a DECLARED type.
// This is the shape the issue's own asymmetry table does not name: the
// INFERRED class spelling (`using var x := src;`) was already clean, because
// `classOfLocal`'s bare-identifier parameter test reached it — but the
// DECLARED spelling read the class off the annotation and disposed the borrow
// (ASan heap-use-after-free, exactly as the interface rows above).
BEZEL_TEST(using_var_borrow_param, declared_class_param_borrow_is_not_disposed_spec_28_10_7387) {
    bezel::test::JitResult r = runAotChecked(R"(
module M;
public class Res
  public constructor() end constructor;
  destructor Console.WriteLine("DTOR"); end destructor;
end Res;
public class Program
  public static method Take(src: Res)
    using var x: Res := src;
  end Take;
  public static async method Main(): Task
    var src: Res := new Res();
    Take(src);
    src.Dispose();
    Console.WriteLine("END");
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "DTOR\nEND\n");
}

// fix #7387 (§28.10, v0.345): the INFERRED class-typed parameter — the
// CONTROL. This shape was already clean before the fix (the issue's own
// asymmetry table), and it must STAY clean: the fix is a classification fix
// (BORROWED vs OWNED), not a "dispose nothing" fix.
BEZEL_TEST(using_var_borrow_param, inferred_class_param_borrow_is_not_disposed_spec_28_10_7387) {
    bezel::test::JitResult r = runAotChecked(R"(
module M;
public class Res
  public constructor() end constructor;
  destructor Console.WriteLine("DTOR"); end destructor;
end Res;
public class Program
  public static method Take(src: Res)
    using var x := src;
  end Take;
  public static async method Main(): Task
    var src: Res := new Res();
    Take(src);
    src.Dispose();
    Console.WriteLine("END");
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "DTOR\nEND\n");
}

// fix #7387 (§28.10, v0.345): the OWNED parameter — the CONTROL that says the
// fix is a CLASSIFICATION fix. §28.3's K1 gives an `owned` parameter an OWNED
// classification, so the value is discharged exactly ONCE — by the parameter's
// own §28.3 K3.14 auto-drop (the `using var` alias is the ownership pass's
// #3253 borrow-pin, so it disposes nothing). The observable is therefore
// exactly ONE `DTOR`.
//
// A fix that disposed NOTHING would leave this `DTOR` unprinted and the object
// leaked; a fix that disposed TWICE would print it twice.
BEZEL_TEST(using_var_borrow_param, owned_param_alias_disposes_exactly_once_spec_28_10_7387) {
    bezel::test::JitResult r = runAotChecked(R"(
module M;
public class Res
  public constructor() end constructor;
  destructor Console.WriteLine("DTOR"); end destructor;
end Res;
public class Program
  public static method Take(owned src: IAsyncDisposable)
    using var x := src;
  end Take;
  public static async method Main(): Task
    var src: IAsyncDisposable := new Res();
    Take(move src);
    Console.WriteLine("END");
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "DTOR\nEND\n");
}
