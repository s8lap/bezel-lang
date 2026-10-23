//! case: s11_6_0018
//! doc: spec
//! section: 11.6
//! title: inside a nested type a sibling nested type of the enclosing generic is reached by bare name
//! rule: Nested types of an enclosing type, generic or not, are reached by bare name, since a nested type
//!   is one type and needs no instantiation (§2.7's Through nesting, v0.340); inside `N`'s own body
//!   the enclosing lookup visits `N` and then `C`, and `C` has a nested type of the name `M2`.
//! cites: 2.7, 11.6
//! expect: run
//! stdout: 7
module Conf;

public class C<T>
  public class N
    public class M2
      public method V(): int => 7;
    end M2;

    public method Run(): int
      using var m := new M2();
      return m.V();
    end Run;
  end N;

  public static method Go(): int
    using var n := new N();
    return n.Run();
  end Go;
end C;

public class Program
  public static method Main(): int
    Console.WriteLine(C<int>.Go());
    return 0;
  end Main;
end Program;
