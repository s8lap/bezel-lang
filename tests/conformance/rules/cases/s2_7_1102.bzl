//! case: s2_7_1102
//! doc: spec
//! section: 2.7
//! title: an ancestor property flows through type parameters named P, Guid and Range
//! rule: inside `class Box<Array>`, a bare `Array` is the type parameter ... A type parameter is
//!   found at the link of the declaration that declares it
//! cites: 14.1, 3.6
//! expect: run
//! output: 5
//!   42
//! issue: 6530
module Conf;
public class A<P>
  public property pa: P;
  public constructor(a: P)
    pa := a;
  end constructor;
end A;
public class B<Guid>: A<Guid>
  public constructor(a: Guid)
    base(a);
  end constructor;
end B;
public class C<Range>: B<Range>
  public constructor(a: Range)
    base(a);
  end constructor;
end C;
public class D<Guid>: C<Guid>
  public constructor(a: Guid)
    base(a);
  end constructor;
  public method Get(): Guid
    return pa;
  end Get;
end D;
public class Program
  public static method Main()
    var x := new D<string>("hello");
    Console.WriteLine(x.Get().Length);
    var y := new D<long>(41);
    Console.WriteLine(y.Get() + 1);
  end Main;
end Program;
