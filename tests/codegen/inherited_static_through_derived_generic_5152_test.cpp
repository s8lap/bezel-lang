// Regression tests for issue #5152 — §22.12 ("A static member declared on a
// base class is a member of every class derived from it ... and may be named
// through a derived type: where `class D<T>: B<T>` declares no `_c` of its
// own, `D<string>._c` names the declaring type's cell at the substitution the
// base clause supplies — `B<string>._c` — and the two spellings are one
// cell."), v0.309. This ruling resolves #4974's open spec question (reading
// 2: alias).
//
// THE DEFECT (silent wrong code, #4974's repro). `Emitter::staticFieldGlobal`
// (LlvmBackend_Setup.cpp) resolved the per-instantiation static-field global
// only when the qualifying receiver's type carried the SAME declaration as
// the field's declaring class (`targetTR->declaration == fs->parent`). Naming
// an inherited static through a DERIVED type (`D<string>._c`, `_c` declared
// on `B<T>`, `class D<T>: B<T>`) missed that check — `targetTR->declaration`
// is `D`, not `B` — so the access fell through to the un-substituted
// TEMPLATE-level global (`bz$B$_c`) instead of `B<string>`'s per-instantiation
// cell, minting a second, independent storage cell. #4974's probe printed
// `0\n0\n` (expected `7\n9\n`): the write through `B<string>._c` did not
// reach `D<string>._c`, and the write-back through `D<string>._c` did not
// reach `B<string>._c` either.
//
// THE FIX. `staticFieldGlobal`'s Path B now also handles a receiver whose
// declaration is a (possibly transitive) DERIVED class of the field's
// `fs->parent`: it composes the substitution `fs->parent` receives from the
// base-clause chain via the shared `computeAncestorGenericSubst` helper (the
// same #2840 vehicle #4950's itable-patch and every inherited-member dispatch
// site already use) and keys the per-instantiation cell on THAT
// substitution — so `D<string>._c` and `B<string>._c` resolve to the same
// LLVM global.
//
// Every row asserts exact stdout; each runs on the plain AOT path and again
// under generated-code ASan (runAotChecked).

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

void expectBoth(const char* src, const std::string& expected) {
    JitResult plain = runAot(src);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    BEZEL_EXPECT_EQ(plain.stdoutText, expected);
    JitResult checked = runAotChecked(src);
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 0);
    BEZEL_EXPECT_EQ(checked.stdoutText, expected);
}

}  // namespace

// ─── The #4974 repro, verbatim: write through B, read through D ───────────
// Pre-fix: "0\n0\n".
BEZEL_TEST(inherited_static_through_derived_generic_5152, issue_4974_repro_write_base_read_derived) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public constructor() end constructor;
end B;
public class D<T>: B<T>
  public constructor() base(); end constructor;
end D;
public class Program
  public static method Main(): int
    B<string>._c := 7;
    Console.WriteLine(D<string>._c);
    D<string>._c := 9;
    Console.WriteLine(B<string>._c);
    return 0;
  end Main;
end Program;
)BZL",
               "7\n9\n");
}

// ─── Write-through-derived, read back both ways ────────────────────────────
// `D<string>._c := 9` must be visible through BOTH `B<string>._c` and
// `D<string>._c` — the coordinator's requested WRITE-through-derived row.
BEZEL_TEST(inherited_static_through_derived_generic_5152, write_through_derived_read_back_via_base_and_derived) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public constructor() end constructor;
end B;
public class D<T>: B<T>
  public constructor() base(); end constructor;
end D;
public class Program
  public static method Main(): int
    D<string>._c := 9;
    Console.WriteLine(B<string>._c);
    Console.WriteLine(D<string>._c);
    D<string>._c := D<string>._c + 1;
    Console.WriteLine(B<string>._c);
    return 0;
  end Main;
end Program;
)BZL",
               "9\n9\n10\n");
}

// ─── Non-generic base control: `class E: B<string>` naming `E._c` ─────────
// `B<T>` is generic but `E` is not; `E._c` must still alias `B<string>._c`.
BEZEL_TEST(inherited_static_through_derived_generic_5152, nongeneric_derived_names_base_cell) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public constructor() end constructor;
end B;
public class E: B<string>
  public constructor() base(); end constructor;
end E;
public class Program
  public static method Main(): int
    E._c := 5;
    Console.WriteLine(B<string>._c);
    B<string>._c := 6;
    Console.WriteLine(E._c);
    return 0;
  end Main;
end Program;
)BZL",
               "5\n6\n");
}

// ─── Two distinct instantiations stay separate ─────────────────────────────
// `D<string>._c` and `D<int>._c` must remain independent cells — the fix must
// not collapse every derived access onto one shared global.
BEZEL_TEST(inherited_static_through_derived_generic_5152, two_instantiations_kept_separate) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public constructor() end constructor;
end B;
public class D<T>: B<T>
  public constructor() base(); end constructor;
end D;
public class Program
  public static method Main(): int
    D<string>._c := 7;
    D<int>._c := 3;
    Console.WriteLine(D<string>._c);
    Console.WriteLine(D<int>._c);
    Console.WriteLine(B<string>._c);
    Console.WriteLine(B<int>._c);
    return 0;
  end Main;
end Program;
)BZL",
               "7\n3\n7\n3\n");
}

// ─── A static METHOD declared on the base reads/writes the same cell the
// derived-named field access reaches ────────────────────────────────────
// `B<T>.Bump()` mutates `B<T>._c` from inside the base's own static-method
// body; the coordinator's requested static-method-on-base row. NOTE: calling
// the method itself THROUGH the derived type name (`D<string>.Bump()`) is a
// SEPARATE, pre-existing gap (static-method dispatch through a derived
// generic type name does not resolve `T` correctly — measured
// `1\n11\n11\n` instead of `1\n12\n12\n` on a probe of that shape; not
// `staticFieldGlobal`, out of scope for #5152, reported separately, not
// fixed here) — so this row calls `Bump()` only through the DECLARING type
// `B<string>`, and exercises the #5152 fix by writing/reading the cell
// through the DERIVED type's field spelling `D<string>._c` in between.
BEZEL_TEST(inherited_static_through_derived_generic_5152, base_static_method_shares_cell_with_derived_field_access) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public constructor() end constructor;
  public static method Bump(): int
    B<T>._c := B<T>._c + 1;
    return B<T>._c;
  end Bump;
end B;
public class D<T>: B<T>
  public constructor() base(); end constructor;
end D;
public class Program
  public static method Main(): int
    Console.WriteLine(B<string>.Bump());
    D<string>._c := D<string>._c + 10;
    Console.WriteLine(B<string>.Bump());
    Console.WriteLine(D<string>._c);
    return 0;
  end Main;
end Program;
)BZL",
               "1\n12\n12\n");
}
