// Regression tests for issue #7569 — a class conforming to the BUILT-IN
// `IAsyncDisposable` with a MISSING or `Task<int>`-returning `DisposeAsync`
// was ADMITTED.
//
// THE GOVERNING TEXT.
//
//   spec §11.2 (v0.355), Bezel_Language_Spec.md:2011 — "A member **matches**
//   an interface member when it is declared `public`, has the same name, the
//   same arity (parameter count), the same number of type parameters of its
//   own, the same parameter modifiers ..., parameter types that are exactly
//   equal under the interface's substitution, and a return type that is
//   exactly equal under the interface's substitution".
//
//   spec §11.2 (v0.355), Bezel_Language_Spec.md:2014 — "**No widening
//   elsewhere.** Parameter types other than the nullability case must match
//   exactly. **Return types must match exactly (no covariance in this
//   version).**"
//
//   spec §11.2 (v0.355), Bezel_Language_Spec.md:2031 — "**A mismatch in any
//   position is a compile error at the implementing class.**"
//
//   spec §28.12 (v0.355), Bezel_Language_Spec.md:8427-8429 — the interface's
//   own declaration:
//
//       interface IAsyncDisposable: IDisposable
//         method DisposeAsync(): Task;
//       end IAsyncDisposable;
//
//   and :8432 — "`DisposeAsync` is declared without an `async` modifier on the
//   interface; `async` is an implementation-side modifier and never appears on
//   interface method declarations. Implementations add `async` to the matching
//   method and use `await` freely."
//
// THE DEFECT.
//
//   The built-in `IAsyncDisposable` is a NULL-DECLARATION built-in interface:
//   it has no user `TypeDecl`, hence no `TypeSymbol` and no `MethodSymbol` for
//   the member it declares. The §11.2 witness
//   (`Resolver::checkAbstractImplementations`, src/sema/TypeResolution_Checks.cpp)
//   implements the matching rule by walking a class's BASE-TYPE closure
//   (`TypeSymbol::baseTypes`) and reading each reached interface's MEMBERS —
//   so the `IAsyncDisposable` edge, which is carried on the OTHER carriage
//   (`TypeSymbol::nonSymbolBaseRefs`, the declared bases that carry no user
//   `TypeSymbol`), is INVISIBLE to it. There is no interface symbol to read a
//   member from, so the class's own `DisposeAsync` was never compared with
//   anything.
//
//   MEASURED on r30-c1 (2e09616d), the two shapes the issue names:
//
//     (a) `class Res: IAsyncDisposable` with NO `DisposeAsync` — COMPILES;
//     (b) `class Res: IAsyncDisposable` with `DisposeAsync(): Task<int>` —
//         COMPILES.
//
//   (b) is also a latent wrong-shape dispatch, not merely a §11.2 formality:
//   codegen assumes the erased `Task` ("Result type is null (DisposeAsync
//   returns Task, not Task<T>)" — src/codegen/LlvmBackend_Async.cpp).
//
// THE ORACLE.
//
//   A rejection is pinned by the DIAGNOSTIC'S TEXT at a REAL LOCATION — never
//   by "the program did not compile" and never by an exit code.
//
// THE CONTROLS.
//
//   The negative controls are the rows a NARROWED check would break: a class
//   with the CORRECT `DisposeAsync(): Task` still compiles and runs; the
//   member may be INHERITED from a base class; an ordinary method named
//   `DisposeAsync` on a class that does NOT list `IAsyncDisposable` is still
//   fine; the `IDisposable` twin is still admitted (its `Dispose()` is
//   compiler-generated from the `destructor`, §28.2 — the user may not
//   declare one); §11.2's check for OTHER interfaces still fires (the
//   `IComparable` row); and an ABSTRACT class may defer the obligation to its
//   concrete subclass (§12.5).
//
// Cite: language spec §11.2 / §28.12 / §12.5 / §28.2 (v0.355); stdlib §2.10
// for the built-in interface's declaration.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot, runAotChecked
#include "test_harness.hpp"

#include <string>
#include <vector>

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Builds a program from `lines`; the 1-based line number of `lines[i]` is
// i + 1, so a row can name the line it expects the diagnostic on without
// counting newlines by hand.
std::string prog(const std::vector<std::string>& lines) {
    std::string s;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        s += lines[i];
        s += "\n";
    }
    return s;
}

// The oracle: the program was REJECTED AT COMPILE TIME, and the diagnostic
// carrying the expected TEXT sits at the expected REAL LOCATION.
bool rejectedAt(const std::string& program, int line, int col,
                const char* text) {
    JitResult r = runAot(program);
    if (!bezel::test::failedAtCompileTimeWithRealLocation(r)) return false;
    const std::string needle =
        ":" + std::to_string(line) + ":" + std::to_string(col) + ": ";
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        std::size_t pos = 0;
        while ((pos = d.message.find(needle, pos)) != std::string::npos) {
            const std::size_t start = pos + needle.size();
            const std::size_t eol = d.message.find('\n', start);
            const std::string one =
                d.message.substr(start, eol == std::string::npos
                                          ? std::string::npos
                                          : eol - start);
            if (one.find(text) != std::string::npos) return true;
            pos += needle.size();
        }
    }
    return false;
}

// The control oracle: the program compiles, links, runs, exits 0 and prints
// exactly `expectedStdout`.
void expectRunsClean(const std::string& program,
                     const std::string& expectedStdout) {
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(bezel::test::diagnosticMessageContains(r, "sanitizer fired"));
    BEZEL_EXPECT_FALSE(bezel::test::diagnosticMessageContains(r, "AddressSanitizer"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedStdout);
}

// The §11.2 wording the missing-member diagnostic carries.
const char* kMissingMember =
    "does not implement interface member `DisposeAsync` of "
    "`IAsyncDisposable`";

// The §11.2 wording the return-type diagnostic carries.
const char* kReturnMismatch =
    "does not match interface `IAsyncDisposable`'s return type";

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// REJECT rows — §11.2 / §28.12 (v0.355).
// ═══════════════════════════════════════════════════════════════════════════

// SHAPE (a) — THE ISSUE'S FIRST PROBE. `class Res: IAsyncDisposable` with NO
// `DisposeAsync` at all. §11.2: "A member matches an interface member when
// it ... has the same name ..." — with no member there is no match, and
// §11.2: "A mismatch in any position is a compile error at the implementing
// class." MEASURED on the pre-fix tip: this COMPILED.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           missing_dispose_async_is_rejected_spec_11_2) {
    const std::string p = prog({
        "module Conf;",
        "public class Res: IAsyncDisposable",
        "  public constructor() end constructor;",
        "end Res;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(p, 2, 8, kMissingMember));
}

// SHAPE (b) — THE ISSUE'S SECOND PROBE. `class Res: IAsyncDisposable` with
// `DisposeAsync(): Task<int>`. §11.2: "Return types must match exactly (no
// covariance in this version)" — the interface declares `Task`, the
// implementation declares `Task<int>`. MEASURED on the pre-fix tip: this
// COMPILED.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           task_int_returning_dispose_async_is_rejected_spec_11_2) {
    const std::string p = prog({
        "module Conf;",
        "public class Res: IAsyncDisposable",
        "  public constructor() end constructor;",
        "  public async method DisposeAsync(): Task<int>",
        "    await Task.Delay(0);",
        "    return 1;",
        "  end DisposeAsync;",
        "end Res;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(p, 4, 3, kReturnMismatch));
}

// The NULLABILITY row: `Task?` is not "exactly equal" to `Task` (§11.2's
// rule is exact on returns — "no covariance in this version", and the
// nullability widening §11.2 grants is a PARAMETER rule only). MEASURED on
// the pre-fix tip: an `async` spelling was rejected by §30.1's
// "an `async` method must declare a `Task`, `Task<T>`, or `OwningTask<T>`
// return type" instead — a different rule, and one that does not fire at all
// for a NON-`async` implementation, which is why this row is spelled
// non-`async`.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           nullable_task_returning_dispose_async_is_rejected_spec_11_2) {
    const std::string p = prog({
        "module Conf;",
        "public class Res: IAsyncDisposable",
        "  public constructor() end constructor;",
        "  public method DisposeAsync(): Task?",
        "    return null;",
        "  end DisposeAsync;",
        "end Res;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(p, 4, 3, kReturnMismatch));
}

// The NO-RETURN-TYPE row: the interface declares `method DisposeAsync():
// Task;`, so a member that declares no return type at all does not match.
// MEASURED on the pre-fix tip: this COMPILED.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           dispose_async_without_a_return_type_is_rejected_spec_11_2) {
    const std::string p = prog({
        "module Conf;",
        "public class Res: IAsyncDisposable",
        "  public constructor() end constructor;",
        "  public method DisposeAsync()",
        "  end DisposeAsync;",
        "end Res;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(p, 4, 3, kReturnMismatch));
}

// The ARITY row: the interface declares a member with NO parameters, so a
// one-parameter `DisposeAsync` does not match it (§11.2: "the same arity
// (parameter count)"). MEASURED on the pre-fix tip: this COMPILED.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           dispose_async_with_a_parameter_is_rejected_spec_11_2) {
    const std::string p = prog({
        "module Conf;",
        "public class Res: IAsyncDisposable",
        "  public constructor() end constructor;",
        "  public method DisposeAsync(x: int)",
        "  end DisposeAsync;",
        "end Res;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(p, 2, 8,
                                  "with the required 0-parameter signature"));
}

// The INHERITED-CONFORMANCE row: the class conforms to a USER interface
// that extends the built-in `IAsyncDisposable`. The obligation is the same
// one — the built-in interface's member reaches the class through the
// extending interface's extension clause. MEASURED on the pre-fix tip: this
// COMPILED.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           missing_dispose_async_through_a_user_interface_is_rejected_spec_11_2) {
    const std::string p = prog({
        "module Conf;",
        "public interface IMy: IAsyncDisposable end IMy;",
        "public class Res: IMy",
        "  public constructor() end constructor;",
        "end Res;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(p, 3, 8, kMissingMember));
}

// The NESTED-CLASS row: §11.6's nested types carry the same obligations as
// a module-level one.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           missing_dispose_async_on_a_nested_class_is_rejected_spec_11_2) {
    const std::string p = prog({
        "module Conf;",
        "public class Outer",
        "  public class Inner: IAsyncDisposable",
        "    public constructor() end constructor;",
        "  end Inner;",
        "end Outer;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(p, 3, 10, kMissingMember));
}

// The NEVER-INSTANTIATED row: §11.2's obligation is a DECLARATION-site rule
// ("A mismatch in any position is a compile error at the implementing
// class"), so a class that is never used is rejected exactly like one that
// is. MEASURED on the pre-fix tip: this COMPILED.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           missing_dispose_async_on_an_unused_class_is_rejected_spec_11_2) {
    const std::string p = prog({
        "module Conf;",
        "public class Res: IAsyncDisposable",
        "  public constructor() end constructor;",
        "end Res;",
        "public class Program",
        "  public static method Main()",
        "    Console.WriteLine(\"ok\");",
        "  end Main;",
        "end Program;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(p, 2, 8, kMissingMember));
}

// ═══════════════════════════════════════════════════════════════════════════
// CONTROL rows — the negative controls. A check that made any of these fail
// would reject every conforming class in the corpus.
// ═══════════════════════════════════════════════════════════════════════════

// THE CRITICAL NEGATIVE CONTROL: the CORRECT spelling — `DisposeAsync():
// Task`, the interface's own return type — is still ADMITTED, and the class
// still runs. `async` is an implementation-side modifier (§28.12), not part
// of §11.2's matching rule.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           correct_dispose_async_is_still_admitted_spec_11_2) {
    const std::string p = prog({
        "module Conf;",
        "public class Res: IAsyncDisposable",
        "  public constructor() end constructor;",
        "  public async method DisposeAsync(): Task",
        "    await Task.Delay(0);",
        "  end DisposeAsync;",
        "end Res;",
        "public class Program",
        "  public static method Main()",
        "    Console.WriteLine(\"ok\");",
        "  end Main;",
        "end Program;",
    });
    expectRunsClean(p, "ok\n");
}

// THE INHERITED-MEMBER CONTROL: §11.2 imposes no restriction that the match
// be declared on the implementing type — an ordinary member INHERITANCE
// applies. The base class declares `DisposeAsync(): Task`; the derived class
// lists `IAsyncDisposable` and inherits the member.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           inherited_dispose_async_is_still_admitted_spec_11_2) {
    const std::string p = prog({
        "module Conf;",
        "public class Base",
        "  public async method DisposeAsync(): Task",
        "    await Task.Delay(0);",
        "  end DisposeAsync;",
        "end Base;",
        "public class Res: Base, IAsyncDisposable",
        "  public constructor() end constructor;",
        "end Res;",
        "public class Program",
        "  public static method Main()",
        "    Console.WriteLine(\"ok\");",
        "  end Main;",
        "end Program;",
    });
    expectRunsClean(p, "ok\n");
}

// THE ORDINARY-METHOD CONTROL: a class that declares a `DisposeAsync` but
// does NOT list `IAsyncDisposable` carries no such obligation — the name is
// an ordinary method name there.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           dispose_async_without_the_interface_is_still_admitted_spec_11_2) {
    const std::string p = prog({
        "module Conf;",
        "public class Res",
        "  public constructor() end constructor;",
        "  public async method DisposeAsync(): Task",
        "    await Task.Delay(0);",
        "  end DisposeAsync;",
        "end Res;",
        "public class Program",
        "  public static method Main()",
        "    Console.WriteLine(\"ok\");",
        "  end Main;",
        "end Program;",
    });
    expectRunsClean(p, "ok\n");
}

// THE `IDisposable` TWIN — MEASURED, NOT ASSUMED. `IDisposable`'s `Dispose()`
// is COMPILER-GENERATED from the `destructor` (§28.2: "User code does not
// implement `Dispose()` directly"), so a class listing `IDisposable` and
// declaring no `Dispose` of its own is CONFORMING and must stay admitted.
// The check for `IAsyncDisposable` must not reach it.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           idisposable_twin_without_a_dispose_is_still_admitted_spec_28_2) {
    const std::string p = prog({
        "module Conf;",
        "public class Res: IDisposable",
        "  public constructor() end constructor;",
        "end Res;",
        "public class Program",
        "  public static method Main()",
        "    using var d: IDisposable := new Res();",
        "    Console.WriteLine(\"ok\");",
        "  end Main;",
        "end Program;",
    });
    expectRunsClean(p, "ok\n");
}

// §11.2's check for OTHER interfaces must STILL FIRE — this is the row that
// catches a narrowed check. `IComparable<Res>`'s `CompareTo` must return
// `int`; a `string` return is a §11.2 mismatch and stays a mismatch.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           comparable_return_type_mismatch_is_still_rejected_spec_11_2) {
    const std::string p = prog({
        "module Conf;",
        "public class Res: IComparable<Res>",
        "  public constructor() end constructor;",
        "  public method CompareTo(other: Res): string",
        "    return \"x\";",
        "  end CompareTo;",
        "end Res;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 4, 3,
        "does not match interface `IComparable`'s return type"));
}

// THE SHADOW CONTROL: a USER interface named `IAsyncDisposable` (§2.3
// permits shadowing an auto-imported name) is a DIFFERENT type — the
// built-in's obligation does not apply to it, and the ORDINARY §11.2 path
// (which CAN see it, because a user interface has a TypeSymbol and its
// members) governs it instead. The class below CONFORMS to the shadow, so
// it compiles — and it compiles only if the built-in's check did not fire
// on it (a false reject here would be the over-broad fix).
BEZEL_TEST(sema_async_disposable_conformance_7569,
           a_user_interface_named_i_async_disposable_is_a_different_type_spec_11_2) {
    const std::string p = prog({
        "module Conf;",
        "public interface IAsyncDisposable",
        "  method DisposeAsync(): Task;",
        "end IAsyncDisposable;",
        "public class Res: IAsyncDisposable",
        "  public constructor() end constructor;",
        "  public async method DisposeAsync(): Task",
        "    await Task.Delay(0);",
        "  end DisposeAsync;",
        "end Res;",
        "public class Program",
        "  public static method Main()",
        "    Console.WriteLine(\"ok\");",
        "  end Main;",
        "end Program;",
    });
    expectRunsClean(p, "ok\n");
}

// … and the ORDINARY §11.2 machinery still governs the shadow: a class
// conforming to the USER interface and declaring no `DisposeAsync` is
// rejected by the ordinary "does not implement inherited abstract member"
// rule (the interface's member IS visible to the §11.2 witness) — NOT by
// the built-in's new diagnostic, which does not apply to a shadow.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           the_user_interface_shadow_is_governed_by_the_ordinary_rule_spec_11_2) {
    const std::string p = prog({
        "module Conf;",
        "public interface IAsyncDisposable",
        "  method DisposeAsync(): Task;",
        "end IAsyncDisposable;",
        "public class Res: IAsyncDisposable",
        "  public constructor() end constructor;",
        "end Res;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 5, 8,
        "does not implement inherited abstract member `DisposeAsync`"));
}

// §12.5: an ABSTRACT class need not implement its inherited obligations —
// the concrete subclass carries them. So an abstract class listing
// `IAsyncDisposable` with no `DisposeAsync` is admitted, while its CONCRETE
// subclass — which inherits the conformance — must provide one.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           an_abstract_class_may_defer_the_obligation_spec_12_5) {
    const std::string p = prog({
        "module Conf;",
        "public abstract class Base: IAsyncDisposable",
        "  public constructor() end constructor;",
        "end Base;",
        "public class Program",
        "  public static method Main()",
        "    Console.WriteLine(\"ok\");",
        "  end Main;",
        "end Program;",
    });
    expectRunsClean(p, "ok\n");
}

// … and the concrete subclass of that abstract class is NOT exempt: the
// obligation reaches it through the base class's conformance.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           a_concrete_subclass_of_an_abstract_conformer_must_implement_spec_12_5) {
    const std::string p = prog({
        "module Conf;",
        "public abstract class Base: IAsyncDisposable",
        "  public constructor() end constructor;",
        "end Base;",
        "public class Derived: Base",
        "  public constructor() end constructor;",
        "end Derived;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(p, 5, 8, kMissingMember));
}

// A NON-GENERIC wrong return type is the same §11.2 mismatch as `Task<int>`:
// "Return types must match exactly (no covariance in this version)". MEASURED
// on the pre-fix tip: this COMPILED.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           int_returning_dispose_async_is_rejected_spec_11_2) {
    const std::string p = prog({
        "module Conf;",
        "public class Res: IAsyncDisposable",
        "  public constructor() end constructor;",
        "  public method DisposeAsync(): int",
        "    return 3;",
        "  end DisposeAsync;",
        "end Res;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(p, 4, 3, kReturnMismatch));
}

// THE `owned`-ON-A-MINT-ANNOUNCING-RETURN CONTROL — §11.2's `owned` in
// interface signatures bullet: "Where the return type, nullability stripped,
// is mint-announcing — a `Task`-family or iterable-family type — the
// announcement table's rows 2 and 3 are identical on both the caller and the
// callee side, so the marker announces nothing the declared type has not
// already announced: **either spelling matches, in either direction.**" So
// `owned Task` MATCHES the interface's `Task` and must stay admitted — a
// return-type check that compared the `owned` MARKER would reject every
// conforming class whose cleanup hands back an owned task.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           owned_task_returning_dispose_async_is_still_admitted_spec_11_2) {
    const std::string p = prog({
        "module Conf;",
        "public class Res: IAsyncDisposable",
        "  public constructor() end constructor;",
        "  public async method DisposeAsync(): owned Task",
        "    await Task.Delay(0);",
        "  end DisposeAsync;",
        "end Res;",
        "public class Program",
        "  public static method Main()",
        "    Console.WriteLine(\"ok\");",
        "  end Main;",
        "end Program;",
    });
    expectRunsClean(p, "ok\n");
}

// THE `IDisposable` TWIN, THE OTHER WAY — §28.2: "User code does not
// implement `Dispose()` directly"; the compiler synthesises `Dispose()` from
// the `destructor`. So a class that lists `IDisposable` and DECLARES a
// `Dispose` of its own is rejected — by §28.2, not by §11.2 — and that
// rejection is unaffected by this fix. MEASURED on the pre-fix tip: this was
// ALREADY rejected, with the same diagnostic.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           idisposable_twin_with_a_user_declared_dispose_is_still_rejected_spec_28_2) {
    const std::string p = prog({
        "module Conf;",
        "public class Res: IDisposable",
        "  public constructor() end constructor;",
        "  public method Dispose(): int",
        "    return 3;",
        "  end Dispose;",
        "end Res;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 4, 3, "may not declare a `Dispose` method directly"));
}

// THE `IDisposable` TWIN, THE THIRD WAY — §28.12: "`IAsyncDisposable`
// extends `IDisposable`". A class that lists the built-in `IAsyncDisposable`
// therefore ALSO conforms to `IDisposable`, whose `Dispose()` is
// compiler-generated: the `IAsyncDisposable` obligation is the `DisposeAsync`
// member ALONE, and the synthetic `Dispose` is not a second obligation. The
// class below declares `DisposeAsync` and no `Dispose` — it must stay
// admitted.
BEZEL_TEST(sema_async_disposable_conformance_7569,
           i_async_disposable_does_not_demand_a_user_dispose_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public class Res: IAsyncDisposable",
        "  public constructor() end constructor;",
        "  public async method DisposeAsync(): Task",
        "    await Task.Delay(0);",
        "  end DisposeAsync;",
        "end Res;",
        "public class Program",
        "  public static method Main()",
        "    Console.WriteLine(\"ok\");",
        "  end Main;",
        "end Program;",
    });
    expectRunsClean(p, "ok\n");
}
