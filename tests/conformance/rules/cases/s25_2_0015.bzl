//! case: s25_2_0015
//! doc: spec
//! section: 25.2
//! title: a nullable ArgumentException operand takes the nullable base overload
//! rule: A derived-typed operand satisfies a base-typed parameter by the
//!   ordinary argument rules.
//! cites: 25.2, 22.6, 31.4
//! expect: run
//! output: 1
//! issue: 6004
module Conf;
record V(N: int) public static pure method operator +(a: V, e: Exception?): V => new V(a.N + 1); public static pure method operator +(a: V, e: ArgumentException): V => new V(a.N + 100); end V;
class P
  public static method Main(): int
    var e: ArgumentException? := new ArgumentException("x"); var v := new V(0); v += e; Console.WriteLine(v.N.ToString());
    return 0;
  end Main;
end P;
