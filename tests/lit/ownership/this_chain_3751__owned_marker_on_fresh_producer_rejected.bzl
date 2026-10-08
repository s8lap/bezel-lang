// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #3751, diagnostic-quality half. §22.3's marker economy: "A **fresh
// producer** — a `new` expression, a call returning `owned T`, an operator
// result, or a `move obj._field` — stays unmarked: no live binding is
// invalidated, so there is nothing to announce". Before the fix this was
// rejected with the WRONG branch of the diagnostic ("this parameter position
// does not take ownership") — false, the position IS `owned` — because the
// chain had no formals to consult. It now names the actual rule.
// per §A.4 / proposal 0090 (v0.246): call-site transfer uses `move`, not `owned`.

module M;
class Res
  private _id: int;
  public constructor(id: int) _id := id; end constructor;
  public method Id(): int => _id;
end Res;
class Base(owned r: Res)
  private owned _s: Res := r;
  public constructor(n: int)
    this(move new Res(n));
  end constructor;
  public method Id(): int
    return _s.Id();
  end Id;
end Base;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `move` call-site marker is not written on a fresh producer (a `new` expression or a call returning an owned result) — no live binding is invalidated, so there is nothing to announce; remove `move` (§22.3
