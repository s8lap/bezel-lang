//! case: s11_6_0008
//! doc: spec
//! section: 11.6
//! title: inside the generic enclosing type its nested type is named by its bare name
//! rule: "a type nested in a generic type ... is named only where §2.7's enclosing-type lookup
//!   reaches it" (§11.6, v0.340); inside `C<T>`'s own body the bare `N` is reached by that
//!   lookup.
//! cites: 2.7
//! expect: run
//! stdout: 9
module Conf;

public class C<T>
  public class N
    public method V(): int
      return 9;
    end V;
  end N;

  public static method Run(): int
    using var n: N := new N();
    return n.V();
  end Run;
end C;

public class Program
  public static method Main()
    Console.WriteLine(C<int>.Run());
  end Main;
end Program;
