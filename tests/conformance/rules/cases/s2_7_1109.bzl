//! case: s2_7_1109
//! doc: spec
//! section: 2.7
//! title: an owned generic field survives renaming the parameter to Guid then Range down a chain
//! rule: inside `class Box<Array>`, a bare `Array` is the type parameter ... A type parameter is
//!   found at the link of the declaration that declares it
//! cites: 14.1, 3.6
//! expect: run
//! output: 23
//! issue: 6530
module Conf;
public class A<K>
  public owned D: Dictionary<K, int>;
  public constructor()
    D := new Dictionary<K, int>();
  end constructor;
end A;
public class B<Guid>: A<Guid>
  public constructor()
    base();
  end constructor;
end B;
public class C<Range>: B<Range>
  public constructor()
    base();
  end constructor;
  public method Go(): int
    D[0] := 11;
    D[1] := 12;
    return D[0] + D[1];
  end Go;
end C;
public class Program
  public static method Main()
    using var x := new C<int>();
    Console.WriteLine(x.Go());
  end Main;
end Program;
