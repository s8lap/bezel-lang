//! case: s7_4_0130
//! doc: spec
//! section: 7.4
//! title: A nullable long reassigned null inside the test still reaches Convert.ToString as nullable
//! rule: Reassignment from a nullable-typed expression invalidates the narrowing (§7.4, v0.350). Convert.ToString(value: long) (stdlib §2.3) then receives `long?`.
//! cites: 7.4, 2.3
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method IdL(x: long?): long? return x; end IdL;
  public static method Main()
    var d: long? := IdL(2L);
    if d <> null then
      d := IdL(null);
      Console.WriteLine(Convert.ToString(d));
    end;
  end Main;
end Program;
