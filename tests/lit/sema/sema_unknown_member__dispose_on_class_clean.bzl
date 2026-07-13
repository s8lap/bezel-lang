// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, dispose_on_class_clean).
//
// The gtest's `parseSemaAll` only runs type resolution + expression
// typecheck (not the ownership/borrow pass), so its original source used a
// plain (borrowed) `h: Handle` parameter — legal at that partial pipeline
// because the point under test is member RESOLUTION (does `.Dispose()`
// resolve to a known member), not ownership legality. `bezelc -c` (what
// this lit RUN line drives) runs the FULL pipeline, which additionally
// enforces §28.6 ("Who may call Dispose()") and would reject a Dispose()
// call through a non-`owned` (borrowed) parameter — an orthogonal rule, not
// what this test is about. The parameter is declared `owned` here so the
// full pipeline accepts it while still exercising the same member-
// resolution point (§8.7: `Dispose` is a valid member of every non-static
// class, even with no user-declared override).
//
// §8.7: `Dispose()` is available on every non-static class (IDisposable),
// even with no user-declared Dispose — must not reject.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Handle public _fd: int; end Handle; class C   public method M(owned h: Handle) h.Dispose(); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
