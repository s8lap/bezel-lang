//! case: s2_7_1111
//! doc: spec
//! section: 2.7
//! title: a leaf type parameter named Guid does not capture an ancestor's concrete string argument
//! rule: inside `class Box<Array>`, a bare `Array` is the type parameter ... A type parameter is
//!   found at the link of the declaration that declares it
//! cites: 14.1, 3.6
//! expect: run
//! output: 5
//! issue: 6530
module Conf;
public class A<X>
  public property bv: X;
  public constructor(v: X)
    bv := v;
  end constructor;
end A;
public class B<Y>: A<string>
  public constructor()
    base("hello");
  end constructor;
end B;
public class C<Guid>: B<Guid>
  public constructor()
    base();
  end constructor;
  public method Go(): int
    return bv.Length;
  end Go;
end C;
public class Program
  public static method Main()
    var x := new C<int>();
    Console.WriteLine(x.Go());
  end Main;
end Program;
