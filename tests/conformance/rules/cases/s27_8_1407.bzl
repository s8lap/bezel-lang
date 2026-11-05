//! case: s27_8_1407
//! doc: spec
//! section: 27.8
//! title: an alias-typed parameter beside the method's Guid parameter denotes the module's Guid
//! rule: A type alias is a pure synonym — it does not create a new type.
//! cites: 27.8, 2.7
//! expect: run
//! stdout: 1
module Conf;
type GMap := Dictionary<Guid, int>;
public class U
  public static method F<Guid>(x: Guid, d: GMap): int
    return d.Count;
  end F;
end U;
public class Program
  public static method Main()
    var d: Dictionary<Guid, int> := new Dictionary<Guid, int>();
    d[Guid.NewGuid()] := 5;
    Console.WriteLine(U.F<int>(3, d));
  end Main;
end Program;
