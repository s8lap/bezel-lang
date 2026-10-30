//! case: l4_4_0049
//! doc: stdlib
//! section: 4.4
//! title: an owned return supplied by ToArray() over a borrowed parameter is accepted
//! rule: public method ToArray(): owned Array<T>;
//! cites: 4.4, 28.3.5
//! expect: run
//! output: 300
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class C
  public static method Mk(a: Array<int>): owned Array<int>
    return a.ToArray();
  end Mk;
  public static method Main(): int
    var xs: Array<int> := [1, 2, 3];
    var t := 0;
    for var i in 0..<100 do t += Mk(xs).Count; end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
