//! case: s2_2_0042
//! doc: spec
//! section: 2.2
//! title: a bare name exported by two imported modules is ambiguous
//! rule: "When two imported modules export the same public name, the **bare** name is
//!   ambiguous: using it unqualified is a compile error citing both origins; the fix is
//!   qualification (full module name or alias)" (§2.2, v0.343). Both `LA` and `LB` export a
//!   public type `N` of arity 0, and the file imports both, so the bare `N` in the
//!   annotation `using var n: N` names two origins at once. The inherited nested type
//!   `N<U>` of `K` does not settle it: rule 1 of §2.7 finds the enclosing type's
//!   accessible members first, but a candidate is only reached when its arity the
//!   reference fixes, and `N<U>` has arity 1 while the bare reference fixes arity 0
//!   (§2.2, v0.343) — so the search reaches the two imported names, and they collide.
//! cites: 2.2, 2.7
//! expect: reject
//! diagnostic: bare name `N` is ambiguous
module Conf;

import LA;
import LB;

public class B
  public class N<U>
    public method Get(): int => 8;
  end N;
end B;

public class K: B
  public static method Run(): int
    using var n: N := new N();
    Console.WriteLine(n.Get());
    return 0;
  end Run;
end K;

public class Program
  public static method Main(): int => K.Run();
end Program;
