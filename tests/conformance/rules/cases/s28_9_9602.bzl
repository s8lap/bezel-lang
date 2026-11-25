//! case: s28_9_9602
//! doc: spec
//! section: 28.9
//! title: a boxed value passed on by a lambda to a non-storing method is usable afterwards
//! rule: so a boxed value is an immutable shared reference
//! cites: 28.9, 8.6
//! expect: run
//! stdout: 190
//! issue: 9206
module Conf;

public class Program
  private static method R(o: Object): int => o.ToString().Length;

  public static method Main()
    var f := lambda(x: Object) => R(x);
    var t := 0;
    for var i in 0 ..< 100 do
      var b: Object := i;
      var n := f(b);
      t := t + n;
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
