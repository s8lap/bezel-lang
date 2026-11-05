//! case: s2_7_1106
//! doc: spec
//! section: 2.7
//! title: a record type argument passes through two class type parameters named Guid and Range
//! rule: inside `class Box<Array>`, a bare `Array` is the type parameter ... A type parameter is
//!   found at the link of the declaration that declares it
//! cites: 14.1, 3.6
//! expect: run
//! output: 23
//! issue: 6530
module Conf;
public record Pt(X: int, Y: int);
public class A<X>
  public property pa: X;
  public constructor(a: X)
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
public class Program
  public static method Main()
    var c := new C<Pt>(new Pt(20, 3));
    Console.WriteLine(c.pa.X + c.pa.Y);
  end Main;
end Program;
