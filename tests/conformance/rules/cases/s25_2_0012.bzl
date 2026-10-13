//! case: s25_2_0012
//! doc: spec
//! section: 25.2
//! title: a compound over ArgumentException selects the exact overload when Exception is declared first
//! rule: For a single argument position, one candidate is better than
//!   another when the better candidate requires no implicit conversion
//! cites: 25.2, 22.6
//! expect: run
//! output: 100
//! issue: 6004
module Conf;
record V(N: int) public static pure method operator +(a: V, e: Exception): V => new V(a.N + 1); public static pure method operator +(a: V, e: ArgumentException): V => new V(a.N + 100); end V;
class P
  public static method Main(): int
    using var e := new ArgumentException("x"); var v := new V(0); v += e; Console.WriteLine(v.N.ToString());
    return 0;
  end Main;
end P;
