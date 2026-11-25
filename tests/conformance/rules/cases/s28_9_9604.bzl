//! case: s28_9_9604
//! doc: spec
//! section: 28.9
//! title: a lambda that stores its second boxed argument keeps the stored value readable
//! rule: so a boxed value is an immutable shared reference
//! cites: 28.9, 8.6
//! expect: run
//! output: 190
//!   1000
//!   1099
//! issue: 9206
module Conf;

public class Program
  public static method Main()
    var a := new Array<Object>();
    var f := lambda(p: Object, x: Object) => do a.Add(x); return p.ToString().Length; end;
    var t := 0;
    for var i in 0 ..< 100 do
      var b0: Object := i;
      var b1: Object := i + 1000;
      var n := f(b0, b1);
      t := t + n;
    end for;
    Console.WriteLine(t);
    Console.WriteLine(a[0].ToString());
    Console.WriteLine(a[99].ToString());
  end Main;
end Program;
