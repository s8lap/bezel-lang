//! case: s2_7_1101
//! doc: spec
//! section: 2.7
//! title: type parameters named Guid and Range resolve to the declaring class down a four-level chain
//! rule: inside `class Box<Array>`, a bare `Array` is the type parameter ... A type parameter is
//!   found at the link of the declaration that declares it
//! cites: 14.1, 3.6
//! expect: run
//! output: 12
//!   12
//! issue: 6530
module Conf;
public class A<P, Q>
  public property pa: P;
  public property pq: Q;
  public constructor(a: P, q: Q)
    pa := a;
    pq := q;
  end constructor;
end A;
public class B<Range>: A<string, Range>
  public constructor(q: Range)
    base("hello", q);
  end constructor;
end B;
public class C<Range, Guid>: B<Guid>
  public constructor(q: Guid)
    base(q);
  end constructor;
end C;
public class D<Guid>: C<Guid, long>
  public constructor()
    base(7);
  end constructor;
  public method Go(): long
    return pa.Length + pq;
  end Go;
end D;
public class Program
  public static method Main()
    var x := new D<int>();
    Console.WriteLine(x.Go());
    var y := new D<string>();
    Console.WriteLine(y.Go());
  end Main;
end Program;
