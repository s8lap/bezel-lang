//! case: s12_1_0051
//! doc: spec
//! section: 12.1
//! title: a non-generic override matches the base parameter through an indirect generic base
//! rule: Visibility, return type, parameter types, and parameter modifiers
//!   must match the base member exactly (v0.338, §12.1). `E1` derives
//!   `D<int>`, which derives `B<T>`; the member `Show(a: T)` that `E1`
//!   inherits is `Show(a: int)` under the base-clause arguments (§14.1), so
//!   `override method Show(a: int)` matches it.
//! cites: 12.1, 14.1
//! expect: run
//! issue: 6299
//! stdout: 7
module Conf;

public class B<T>
  public constructor()
  end constructor;

  public virtual method Show(a: T): int => 1;
end B;

public class D<T>: B<T>
  public constructor()
  end constructor;
end D;

public class E1: D<int>
  public constructor()
  end constructor;

  public override method Show(a: int): int => 7;
end E1;

public class Program
  public static method Main(): int
    var b: B<int> := new E1();
    Console.WriteLine(b.Show(3));
    return 0;
  end Main;
end Program;
