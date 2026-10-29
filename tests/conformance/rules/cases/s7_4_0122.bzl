//! case: s7_4_0122
//! doc: spec
//! section: 7.4
//! title: A nullable passed to Math.Abs in the then-branch of `is null` stays nullable
//! rule: In the true branch of `e is null` the variable is known null, not non-null: "x = null or x is null (narrowing the *false* branch)" (§7.4, v0.350) narrows only the other branch, so `Math.Abs(e)` receives a `double?`, which `Abs(x: double)` refuses.
//! cites: 7.4, 2.2
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method IdD(x: double?): double? return x; end IdD;
  public static method Main()
    var e: double? := IdD(2.5);
    if e is null then Console.WriteLine(Math.Abs(e)); end;
  end Main;
end Program;
