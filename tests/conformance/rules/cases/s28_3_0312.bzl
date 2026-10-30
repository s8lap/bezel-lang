//! case: s28_3_0312
//! doc: spec
//! section: 28.3
//! title: borrowed meet borrowed in a ?? is accepted
//! rule: borrowed ⊓ borrowed = **borrowed** with the union of sources; mixed = **rejected** —
//!   equalize the arms first
//! cites: 28.3.4, K7.3
//! expect: run
//! output: 400
module Conf;
import System;
import System.Text;
import System.Collections.Generic;
class C
  public static method Pick(p: StringBuilder?, q: StringBuilder): int
    var r := p ?? q;
    var a: Array<int>? := null;
    var b: Array<int> := [1, 2];
    var m := a ?? b;
    return r.Length + m.Count;
  end Pick;
  public static method Main(): int
    using var s := new StringBuilder("ab");
    var t := 0;
    for var i in 0..<100 do t += Pick(null, s); end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
