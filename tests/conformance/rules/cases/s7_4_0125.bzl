//! case: s7_4_0125
//! doc: spec
//! section: 7.4
//! title: Reassigning a narrowed local from a nullable expression invalidates the narrowing
//! rule: "The narrowing of a variable is invalidated when ... the variable is reassigned from a nullable-typed expression" (§7.4, v0.350). After `d := null;` the `Math.Abs(d)` argument is `double?`.
//! cites: 7.4, 2.2
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method IdD(x: double?): double? return x; end IdD;
  public static method Main()
    var d: double? := IdD(2.5);
    if d <> null then
      d := null;
      var r: double := Math.Abs(d);
      Console.WriteLine(r);
    end;
  end Main;
end Program;
