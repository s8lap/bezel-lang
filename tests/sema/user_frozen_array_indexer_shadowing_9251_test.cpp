// Regression tests for issue #9251 — a USER `class FrozenArray<T>`'s own
// WRITABLE indexer was rejected by the BUILT-IN `FrozenArray<T>`'s read-only
// indexer rule (stdlib §4.19 / §30.14).
//
// THE DEFECT. `ExprTC::checkPropertyWriteTarget` (src/sema/ExprTypecheck.cpp)
// carries the fix #1723 rule: a write through a `FrozenArray<T>`'s indexer is
// a compile error, because stdlib §4.19 (v0.359) declares
//
//     public property [i: int]: T
//       get;                       // bounds-checked; ...
//     end;
//
// — a GETTER-ONLY indexer on a deeply immutable type ("its contents can never
// change after construction"), so §30.14's freezing semantics forbid a
// programmatic mutation.
//
// The rule's receiver predicate keyed on the receiver's HEAD NAME alone, so a
// USER class that reuses the name matched it. MEASURED on the base r30-c1
// (`009b0c5de`), on the issue's own program:
//
//     f[0] := 3;   ->  error: cannot assign through a `FrozenArray<T>` indexer
//                       — it is deeply immutable and declares no setter
//                       (§4.19 / §30.14)
//
// — a FALSE REJECTION of a legal program: the user's class is not deeply
// immutable and DOES declare a setter.
//
// WHY THE USER'S CLASS WINS THE NAME. Language spec §2.3 (v0.359,
// *Always-available names*) puts `FrozenArray<T>` in the "Auto-imported module:
// `System.Collections.Generic`" row, whose *Shadowable?* column reads "Yes",
// and the paragraph after the table says "every other always-available name
// may be shadowed by local declarations under the normal name-resolution
// rules". §2.7 rule 1 (v0.359) resolves the bare name first — "local scope,
// then the enclosing type's accessible members, THEN THE CURRENT MODULE'S
// DECLARATIONS, then unambiguous imported bare names" — so a user's
// `class FrozenArray<T>` in the current module takes the bare name and the
// built-in stays reachable only as `System.Collections.Generic.FrozenArray<T>`.
// The standard library's type is then not the type in hand, and §4.19's
// read-only-indexer rule — a property of THAT type — does not reach the user's.
//
// THE FIX. The rule now asks the RESOLVED DECLARATION, not the name:
// `isUserDeclaredTypeSym` (src/sema/Symbol.hpp:502) is the house discriminator
// for "did a user declaration take the spelling?" (§2.3 / #5272) — a
// `builtinClassSym` is shaped `decl == nullptr && moduleName.empty()`, so a
// user's `TypeSymbol` is its exact complement (a real `ast::TypeDecl`
// in-module, or a populated `moduleName` when imported). A null declaration is
// NOT a user type (the built-in's own declaration-less Named shape answers
// false), so the built-in's rejection is unchanged in every shape. This is the
// sibling `Length` rule's own discriminator, in the same function (fix #9214,
// stdlib §4.19 / §23.3).
//
// Spec: stdlib §4.19 (the `FrozenArray<T>` declaration block — the indexer's
// `get`-only half), language spec §30.14 (the freezing semantics), §2.3 (the
// shadowable always-available names), §2.7 rule 1 (the bare-name resolution
// order), §2.2 (a current-module declaration shadows an imported bare name),
// §23.3 (a property with no setter is read-only — the ORDINARY rule that
// governs the user's own getter-only indexer), §17.6 (read-only locations
// cannot be incremented). Spec revision v0.359.
//
// Model: none — the spec decides the case (stdlib §4.19 + §2.3 / §2.7).

#include <initializer_list>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True iff an ERROR diagnostic contains every one of `needles`.
bool errorSays(const JitResult& r,
               std::initializer_list<const char*> needles) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        bool all = true;
        for (const char* n : needles)
            if (d.message.find(n) == std::string::npos) { all = false; break; }
        if (all) return true;
    }
    return false;
}

// True iff some ERROR diagnostic contains `needle`.
bool errorMentions(const JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The issue's own program, VERBATIM in shape: a user `class FrozenArray<T>`
// with a WRITABLE indexer, and a write through it. §2.3 / §2.7 rule 1: the
// user's class wins the bare name, so this is an ordinary class and its own
// indexer answers — the write is legal.
const char* kUserWritable =
    R"(module P; import System;
public class FrozenArray<T>
  private _items: Array<T>;
  public constructor(items: Array<T>) this._items := items; end constructor;
  public property [i: int]: T
    get => this._items[i];
    set => do this._items[i] := value; end;
  end;
end FrozenArray;
public class Program
  public static method Main(): int
    var s := new Array<int>(); s.Add(7);
    var f := new FrozenArray<int>(s);
    f[0] := 3;
    Console.WriteLine(f[0]);
    return 0;
  end Main;
end Program;
)";

// The same user class reached through a FIELD's type — the receiver TypeRef is
// the field's type, a different spelling of the same type.
const char* kUserWritableThroughField =
    R"(module P; import System;
public class FrozenArray<T>
  private _items: Array<T>;
  public constructor(items: Array<T>) this._items := items; end constructor;
  public property [i: int]: T
    get => this._items[i];
    set => do this._items[i] := value; end;
  end;
end FrozenArray;
public class Holder
  public F: FrozenArray<int>;
  public constructor(f: FrozenArray<int>) this.F := f; end constructor;
end Holder;
public class Program
  public static method Main(): int
    var s := new Array<int>(); s.Add(7);
    var h := new Holder(new FrozenArray<int>(s));
    h.F[0] := 3;
    Console.WriteLine(h.F[0]);
    return 0;
  end Main;
end Program;
)";

// The GETTER-ONLY twin: the user's own class, but its indexer declares no
// setter — so the write is still a compile error, and the ORDINARY read-only
// rule (§23.3) is the one that must answer. The built-in's §4.19 / §30.14
// rule has nothing to say about a user's class.
const char* kUserGetterOnly =
    R"(module P; import System;
public class FrozenArray<T>
  private _items: Array<T>;
  public constructor(items: Array<T>) this._items := items; end constructor;
  public property [i: int]: T
    get => this._items[i];
  end;
end FrozenArray;
public class Program
  public static method Main(): int
    var s := new Array<int>(); s.Add(7);
    var f := new FrozenArray<int>(s);
    f[0] := 3;
    Console.WriteLine(f[0]);
    return 0;
  end Main;
end Program;
)";

}  // namespace

// §2.3 / §2.7 rule 1 (v0.359): the user's `class FrozenArray<T>` wins the
// bare name, so its own WRITABLE indexer is an assignable location — the
// program compiles, runs, and the write lands (7 becomes 3).
BEZEL_TEST(user_frozen_array_indexer_shadowing_9251,
           user_writable_indexer_assignment_admitted_spec_2_3) {
    auto r = runAot(kUserWritable);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

// The same write through a FIELD whose type is the user's class: the
// receiver TypeRef is a different shape (the field's type), and the built-in's
// rule must not reach it either.
BEZEL_TEST(user_frozen_array_indexer_shadowing_9251,
           user_writable_indexer_assignment_through_field_admitted_spec_2_3) {
    auto r = runAot(kUserWritableThroughField);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

// §23.3 (v0.359): a property with no setter is read-only — the ORDINARY rule
// answers for the user's GETTER-ONLY indexer, and the built-in's §4.19 /
// §30.14 rule must NOT also fire (on the base it fired as well, and its
// "deeply immutable" wording named the wrong type).
BEZEL_TEST(user_frozen_array_indexer_shadowing_9251,
           user_getonly_indexer_rejected_by_23_3_only_spec_23_3) {
    auto r = runAot(kUserGetterOnly);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, {"read-only indexer", "no setter", "23.3"}));
    // The built-in's rule must not reach a user's class.
    BEZEL_EXPECT_FALSE(errorMentions(r, "deeply immutable"));
    BEZEL_EXPECT_FALSE(errorMentions(r, "4.19"));
}
