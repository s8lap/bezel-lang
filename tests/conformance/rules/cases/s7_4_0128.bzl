//! case: s7_4_0128
//! doc: spec
//! section: 7.4
//! title: A non-readonly field is never narrowed, so Math.Abs of it is refused
//! rule: "Narrowing applies to locals -- and `readonly` fields -- only" (§7.4, v0.350). A mutable `double?` field tested with `<> null` stays `double?` at the use.
//! cites: 7.4, 2.2
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Box
  public f: double?;
  public constructor(a: double?) this.f := a; end constructor;
end Box;

public class Program
  public static method IdD(x: double?): double? return x; end IdD;
  public static method Main()
    var c := new Box(IdD(2.5));
    if c.f <> null then Console.WriteLine(Math.Abs(c.f)); end;
  end Main;
end Program;
