//! case: s25_2_0014
//! doc: spec
//! section: 25.2
//! title: a nullable Exception operand selects the Exception? overload
//! rule: For a single argument position, one candidate is better than
//!   another when the better candidate requires no implicit conversion
//! cites: 25.2, 22.6
//! expect: run
//! output: 3
//! issue: 6004
module Conf;
record V(N: int) public static pure method operator +(a: V, e: Exception): V => new V(a.N + 1); public static pure method operator +(a: V, e: Exception?): V => new V(a.N + 3); end V;
class P
  public static method Main(): int
    var e: Exception? := new Exception("x"); var v := new V(0); v += e; Console.WriteLine(v.N.ToString());
    return 0;
  end Main;
end P;
