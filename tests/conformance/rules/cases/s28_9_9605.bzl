//! case: s28_9_9605
//! doc: spec
//! section: 28.9
//! title: a lambda that stores an alias of its boxed argument keeps the stored value readable
//! rule: so a boxed value is an immutable shared reference
//! cites: 28.9, 8.6
//! expect: run
//! output: 100
//!   0
//!   99
//! issue: 9206
module Conf;

public class Program
  public static method Main()
    var a := new Array<Object>();
    var f := lambda(x: Object) => do var y: Object := x; a.Add(y); return 1; end;
    var t := 0;
    for var i in 0 ..< 100 do
      var b: Object := i;
      var n := f(b);
      t := t + n;
    end for;
    Console.WriteLine(t);
    Console.WriteLine(a[0].ToString());
    Console.WriteLine(a[99].ToString());
  end Main;
end Program;
