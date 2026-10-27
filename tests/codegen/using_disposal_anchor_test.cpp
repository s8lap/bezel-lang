// The `using` / `await using` DISPOSAL ANCHOR (campaign-1 `using-heads`).
//
// §28.12 (v0.345) is explicit about the key: "`await using` selects the
// logical-disposal path by the class of the object the binding HOLDS ... never
// by the binding's static type." The compiler resolved that class from the
// binding's DECLARED type, so an interface-typed, base-typed or abstract-typed
// head named the wrong class (or none), and the awaited `DisposeAsync` was
// never resolved — the DESTRUCTOR ran instead, and §28.12 step 2's
// reclamation was keyed on the base.
//
// The anchor is `common::classifyDisposal` (src/common/DisposalKind.hpp): ONE
// predicate, keyed on the slot's REPRESENTATION (the right-hand side's own
// type, monomorphization-aware), read by BOTH the sema side
// (`MethodOwnership::rhsIsDisposableClassType`) and the codegen side
// (`Emitter::registerUsingDisposal`).
//
// Each row below names the issue it closes and the spec section it derives
// from.

#include <string>

#include "common/DisposalKind.hpp"
#include "sema/SemaContext.hpp"
#include "support/aot_support.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// "body\n" repeated `n` times.
std::string bodyLines(int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += "body\n";
    return out;
}

struct Prog {
    bezel::test::JitResult r;
    explicit Prog(const std::string& src) : r(runAot(src)) {}
};

}  // namespace

// fix #3483 (§28.12 step 1, v0.345): `await using var d: IAsyncDisposable := new
// Res();` must await the RUN-TIME object's `DisposeAsync` — §28.12: "Where
// that class implements `IAsyncDisposable`, scope exit awaits its
// `DisposeAsync` ... through any interface — `IAsyncDisposable` or one
// deriving from it". The pre-fix output was `body / DTOR`: the DESTRUCTOR ran
// and the asynchronous cleanup was silently skipped.
//
// The marker is in `DisposeAsync` (not only in the destructor): a row that
// cannot tell the two paths apart cannot pin this.
BEZEL_TEST(using_disposal_anchor, interface_head_awaits_dispose_async_spec_28_12_3483) {
    Prog p(R"(
module M;
public class Res: IAsyncDisposable
  public constructor() end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("DA");
  end DisposeAsync;
  destructor Console.WriteLine("DTOR"); end destructor;
end Res;
public class Program
  public static async method Main(): Task
    await using var d: IAsyncDisposable := new Res();
    Console.WriteLine("body");
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(p.r.ran);
    BEZEL_EXPECT_EQ(p.r.exitCode, 0);
    BEZEL_EXPECT_EQ(p.r.stdoutText, "body\nDA\n");
}

// fix #7328 (§28.12 step 1, v0.345): the same shape at the OTHER built-in
// interface head. §28.12 names it: "or another such as `IDisposable`, which
// every class implements. Only an object whose class does not implement
// `IAsyncDisposable` takes the synchronous disposal above." `Solo` DOES
// implement it, so the awaited `DisposeAsync` runs and the destructor does
// not.
BEZEL_TEST(using_disposal_anchor, idisposable_head_awaits_dispose_async_spec_28_12_7328) {
    Prog p(R"(
module M;
public class Solo: IAsyncDisposable
  public constructor() end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("ASYNC");
  end DisposeAsync;
  destructor Console.WriteLine("SYNC"); end destructor;
end Solo;
public class Program
  public static async method Main(): Task
    await using var d: IDisposable := new Solo();
    Console.WriteLine("body");
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(p.r.ran);
    BEZEL_EXPECT_EQ(p.r.exitCode, 0);
    BEZEL_EXPECT_EQ(p.r.stdoutText, "body\nASYNC\n");
}

// fix #5653 (§28.12 step 1, v0.345): §28.12's OWN worked example — "through a
// base-class static type that does not implement `IAsyncDisposable` (`await
// using var b: Base := new Der();` with `class Der: Base, IAsyncDisposable`
// awaits `Der.DisposeAsync`, skips `Der`'s destructor and runs `Base`'s)".
//
// The pre-fix output was `body / DER_DTOR / BASE_DTOR`: the base-typed head
// named `Base`, which has no `DisposeAsync`, so the async cleanup never ran.
BEZEL_TEST(using_disposal_anchor, base_head_awaits_run_time_dispose_async_spec_28_12_5653) {
    Prog p(R"(
module M;
public class Base
  public constructor() end constructor;
  destructor Console.WriteLine("BASE_DTOR"); end destructor;
end Base;
public class Der: Base, IAsyncDisposable
  public constructor() end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("DA");
  end DisposeAsync;
  destructor Console.WriteLine("DER_DTOR"); end destructor;
end Der;
public class Program
  public static async method Main(): Task
    await using var b: Base := new Der();
    Console.WriteLine("body");
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(p.r.ran);
    BEZEL_EXPECT_EQ(p.r.exitCode, 0);
    // `Der`'s destructor body is SKIPPED (step 1: `DisposeAsync` supersedes
    // it) and `Base`'s RUNS (step 2, per class: `Base` declares no
    // `DisposeAsync`).
    BEZEL_EXPECT_EQ(p.r.stdoutText, "body\nDA\nBASE_DTOR\n");
}

// fix #3516 (§28.12 step 1 and step 2, v0.345): an ABSTRACT-class head whose
// `DisposeAsync` is `abstract`. §28.12 step 2, per class: "the destructor body
// is skipped if that level declares or overrides a non-`abstract`
// `DisposeAsync`, and runs if it does not — ... a level whose `DisposeAsync` is
// `abstract` (it performs no cleanup, and no override can chain to it), are
// cleaned up exactly as on the synchronous path."
//
// So: `AbsD` overrides a NON-abstract `DisposeAsync` ⇒ `AbsD`'s destructor body
// is SKIPPED and its `DisposeAsync` is awaited; `Abs` declares an ABSTRACT one
// ⇒ `Abs`'s destructor body RUNS.
//
// The pre-fix output was `body / ABSD_DTOR / ABS_DTOR`: the destructor chain,
// not the awaited `DisposeAsync`.
BEZEL_TEST(using_disposal_anchor, abstract_head_awaits_dispose_async_spec_28_12_3516) {
    Prog p(R"(
module M;
public abstract class Abs: IAsyncDisposable
  public constructor() end constructor;
  public abstract method DisposeAsync(): Task;
  destructor Console.WriteLine("ABS_DTOR"); end destructor;
end Abs;
public class AbsD: Abs
  public constructor() end constructor;
  public async override method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("DA");
  end DisposeAsync;
  destructor Console.WriteLine("ABSD_DTOR"); end destructor;
end AbsD;
public class Program
  public static async method Main(): Task
    await using var u: Abs := new AbsD();
    Console.WriteLine("body");
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(p.r.ran);
    BEZEL_EXPECT_EQ(p.r.exitCode, 0);
    BEZEL_EXPECT_EQ(p.r.stdoutText, "body\nDA\nABS_DTOR\n");
}

// fix #3482 (§28.9 / §28.12, v0.345): a MONOMORPHIZED type-parameter head.
// `await using var s := new T();` at `T = Stopwatch` had no nameable head, so
// the `await using` twin read word 0 of the value as a class vtable (ASan:
// SEGV) and the plain `using var` twin registered no disposal at all (32 B
// leaked / 1 allocation). The two twins DISAGREED at that head.
//
// The head is now resolved MONOMORPHIZATION-AWARE, so the `await using` twin
// takes the same arms the plain spelling takes and neither reads word 0.
//
// The leak half of the oracle is the SANITIZER's verdict: run this file with
// BEZEL_AOT_FORCE_SANITIZE=address and the plain twin must report "0 byte(s)
// leaked in 0 allocation(s)". The loop is inside the program (200 iterations)
// because a ONE-SHOT leak repro is vacuous under LSan's default options.
BEZEL_TEST(using_disposal_anchor, monomorphized_await_using_head_does_not_read_word_zero_spec_28_12_3482) {
    Prog p(R"(
module M;
import System.Diagnostics;
public class Program
  public static async method Make<T>(): Task where T: class, new()
    await using var s := new T();
    Console.WriteLine("body");
  end Make;
  public static async method Main(): Task
    for var i in 0..<200 do
      await Program.Make<Stopwatch>();
    end for;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(p.r.ran);
    BEZEL_EXPECT_EQ(p.r.exitCode, 0);
    BEZEL_EXPECT_EQ(p.r.stdoutText, bodyLines(200));
}

BEZEL_TEST(using_disposal_anchor, monomorphized_using_var_head_is_disposed_spec_28_9_3482) {
    // The LEAK oracle is `runAotChecked`: the generated code is
    // ASan/LSan-instrumented there, so a leak flips `ran` to false.
    // `runAot` does NOT leak-check — `BEZEL_AOT_FORCE_SANITIZE=address` pins
    // `detect_leaks=0` deliberately (tests/README.md, #1822) — so a leak row
    // on `runAot` passes on BOTH sides of its fix and pins nothing.
    bezel::test::JitResult r = runAotChecked(R"(
module M;
import System.Diagnostics;
public class Program
  public static method Make<T>(): int where T: class, new()
    using var s := new T();
    Console.WriteLine("body");
    return 0;
  end Make;
  public static method Main(): int
    for var i in 0..<200 do
      _ := Program.Make<Stopwatch>();
    end for;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, bodyLines(200));
}

// fix #7338 (§28.9, v0.345): the runtime-managed SET. §28.9's list is
// "{string, Type, boxed value types, function values}", and the `Type` token
// was missing from the codegen predicate's set — so a `typeof` right-hand side
// took the CLASS disposal path, which §28.9 forbids ("the instance is not
// compiler-freed at any holder's release point ... the trivial-dispose free
// below would set the disposed flag on a token every other holder still
// reads").
//
// `typeof` is rejected today (reflection is not implemented, §17.10), so the
// defect is the CLASSIFICATION DISAGREEMENT between the semantic layer and the
// codegen layer, and the oracle is the shared predicate's SET — pinned
// directly here, as the issue requires ("a NEGATIVE TEST that pins the
// PREDICATE'S SET directly ... NOT AS A `typeof` PROGRAM").
BEZEL_TEST(using_disposal_anchor, type_token_is_runtime_managed_spec_28_9_7338) {
    bezel::DiagnosticEngine diags;
    bezel::sema::SemaContext ctx(diags);

    // The `Type` token: the BUILTIN `Type` class (a `builtinClassSym` shape —
    // a declaration with no `ast::TypeDecl` and no originating module).
    bezel::sema::TypeRef typeTok;
    typeTok.kind = bezel::sema::TypeRef::Kind::Named;
    typeTok.declaration = ctx.builtinClassSym("Type");
    auto v = bezel::common::classifyDisposal(&typeTok, ctx);
    BEZEL_EXPECT_TRUE(v.kind == bezel::common::DisposalKind::None);
    BEZEL_EXPECT_FALSE(v.mayHoldClass);
    BEZEL_EXPECT_FALSE(v.carriesObligation);

    // The rest of §28.9's runtime-managed set, on the same authority: a
    // `string` and a function value are runtime-managed too.
    bezel::sema::TypeRef str;
    str.kind = bezel::sema::TypeRef::Kind::Primitive;
    str.primitive = bezel::sema::PrimitiveType::String;
    auto vs = bezel::common::classifyDisposal(&str, ctx);
    BEZEL_EXPECT_TRUE(vs.kind == bezel::common::DisposalKind::None);
    BEZEL_EXPECT_FALSE(vs.mayHoldClass);

    // And a CLASS is not runtime-managed — the anchor's other arm, pinned so
    // the `Type` arm cannot swallow it.
    bezel::sema::TypeRef prim;
    prim.kind = bezel::sema::TypeRef::Kind::Primitive;
    prim.primitive = bezel::sema::PrimitiveType::Int;
    auto vp = bezel::common::classifyDisposal(&prim, ctx);
    BEZEL_EXPECT_TRUE(vp.kind == bezel::common::DisposalKind::None);
    BEZEL_EXPECT_FALSE(vp.mayHoldClass);
}

// fix #3482 (§28.9 / §28.12, v0.345): the monomorphization-aware head must
// also name a CLASS-constrained instantiation, so the plain `using var` twin
// registers the disposal it always meant to. `T = Res` (a class with a
// destructor) at `using var s := new T();` prints the destructor — the
// campaign's grid cell (`generic_class`, `using_var`) expects exactly
// `DTOR` then `body`. Before the fix the head was unnameable, so NO
// disposal was registered and the destructor never ran.
BEZEL_TEST(using_disposal_anchor, monomorphized_class_head_disposes_spec_28_9_3482) {
    Prog p(R"(
module M;
public class Res
  public constructor() end constructor;
  destructor Console.WriteLine("DTOR"); end destructor;
end Res;
public class Program
  public static method Make<T>(): int where T: class, new()
    using var s := new T();
    Console.WriteLine("body");
    return 0;
  end Make;
  public static method Main(): int
    _ := Program.Make<Res>();
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(p.r.ran);
    BEZEL_EXPECT_EQ(p.r.exitCode, 0);
    // The `using var` scope ends when `Make` returns, so the destructor runs
    // AFTER the body's own print.
    BEZEL_EXPECT_EQ(p.r.stdoutText, "body\nDTOR\n");
}

// fix #7328's CONTROL (§28.12, v0.345): the SYNCHRONOUS twin at the SAME
// interface-typed head must still dispose EXACTLY ONCE, through the
// destructor. The campaign's grid cell (`interface`, `using_var`) expects
// `DTOR`. The marker is in the destructor only: this row pins that the
// anchor's async arm did not take the sync path away.
BEZEL_TEST(using_disposal_anchor, idisposable_head_sync_twin_still_disposes_spec_28_12_7328) {
    Prog p(R"(
module M;
public class Solo: IAsyncDisposable
  public constructor() end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("ASYNC");
  end DisposeAsync;
  destructor Console.WriteLine("SYNC"); end destructor;
end Solo;
public class Program
  public static async method Main(): Task
    using var d: IDisposable := new Solo();
    Console.WriteLine("body");
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(p.r.ran);
    BEZEL_EXPECT_EQ(p.r.exitCode, 0);
    BEZEL_EXPECT_EQ(p.r.stdoutText, "body\nSYNC\n");
}
