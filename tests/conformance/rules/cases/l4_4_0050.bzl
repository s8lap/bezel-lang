//! case: l4_4_0050
//! doc: stdlib
//! section: 4.4
//! title: owned returns supplied by Sorted, Reversed, Slice and a range slice over a field are
//!   accepted
//! rule: public method Sorted(): owned Array<T> where T: IComparable<T>;
//! cites: 4.4, 28.3.5
//! expect: run
//! output: 1000
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class H
  public xs: Array<int>;
  public constructor(xs: Array<int>) this.xs := xs; end constructor;
  public method A(): owned Array<int> => this.xs.Sorted();
  public method B(): owned Array<int> => this.xs.Reversed();
  public method S(): owned Array<int> => this.xs.Slice(0..<2);
  public method R(): owned Array<int> => this.xs[1..<3];
end H;
class C
  public static method Main(): int
    var xs: Array<int> := [3, 1, 2];
    using var h := new H(xs);
    var t := 0;
    for var i in 0..<100 do t += h.A().Count + h.B().Count + h.S().Count + h.R().Count; end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
