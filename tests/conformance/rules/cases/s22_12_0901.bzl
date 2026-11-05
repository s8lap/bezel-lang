//! case: s22_12_0901
//! doc: spec
//! section: 22.12
//! title: a bare inherited generic static in an async lambda bumps the declaring instantiation's cell
//! rule: There is one cell per declaring instantiation, whatever type name reaches it.
//! cites: 14.1, 27.5
//! expect: run
//! output: 3
//!   3
//!   3
//!   7
//!   4
//!   3
//! issue: 8127
module Conf;

public class B<T>
  public static _c: int := 0;

  public constructor()
  end constructor;

  public static method Bump(): int
    B<T>._c := B<T>._c + 1;
    return B<T>._c;
  end Bump;
end B;

public class D<T>: B<T>
  public constructor()
    base();
  end constructor;

  public static method Inner(): int
    var f := async lambda(): Task<int> => Bump();
    using var t1 := f();
    using var t2 := f();
    return t1.Result + t2.Result;
  end Inner;

  public method I(): int
    var f := async lambda(): Task<int> => Bump();
    using var t := f();
    return t.Result;
  end I;
end D;

public class E: B<int>
  public constructor()
    base();
  end constructor;

  public static method Inner2(): int
    var f := async lambda(): Task<int> => Bump();
    using var t1 := f();
    using var t2 := f();
    return t1.Result + t2.Result;
  end Inner2;
end E;

public class Program
  public static method Main()
    Console.WriteLine(D<string>.Inner());
    Console.WriteLine(D<int>.Inner());
    using var d := new D<string>();
    Console.WriteLine(d.I());
    Console.WriteLine(E.Inner2());
    Console.WriteLine(B<int>._c);
    Console.WriteLine(B<string>._c);
  end Main;
end Program;
