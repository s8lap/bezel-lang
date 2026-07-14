// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// spec §7.9 (Appendix C.1), v0.196 — non-nullable field definite assignment at
// constructor normal exit. Regression for #1755 (accept case) / #1823.
//
// The non-nullable `_name` is assigned on every NORMAL-exit path: the `1` arm
// diverges (throw — it reaches no normal exit), and the `_` catch-all assigns
// `_name`. The all-paths join intersects over LIVE arms only, so this is
// ACCEPTED. The retired any-path ownership collector false-REJECTED it (#1755).

module M;
sealed class W
  private _name: string;
  public constructor(n: int)
    case n of
      1 => do throw new Exception(); end,
      _ => do _name := "a"; end
    end;
  end constructor;
  public method N(): string return _name; end N;
end W;

// NODIAG-NOT: error
// NODIAG-NOT: §
