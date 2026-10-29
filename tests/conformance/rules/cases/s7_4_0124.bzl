//! case: s7_4_0124
//! doc: spec
//! section: 7.4
//! title: A nullable passed to Math.Abs in the else branch of `<> null` stays nullable
//! rule: Only the true branch of `x <> null` is narrowed (§7.4, v0.350); in the else branch `e` is still `double?` and `Math.Abs(e)` is refused.
//! cites: 7.4, 2.2
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method IdD(x: double?): double? return x; end IdD;
  public static method Main()
    var e: double? := IdD(2.5);
    if e <> null then Console.WriteLine(1); else Console.WriteLine(Math.Abs(e)); end;
  end Main;
end Program;
