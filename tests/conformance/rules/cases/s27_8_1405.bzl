//! case: s27_8_1405
//! doc: spec
//! section: 27.8
//! title: an alias of an alias denotes the module's Guid at a method with a type parameter Guid
//! rule: A type alias is a pure synonym — it does not create a new type.
//! cites: 27.8, 2.7
//! expect: run
//! stdout: 1
module Conf;
type GMap := Dictionary<Guid, int>;
type GMap2 := GMap;
public class U
  public static method F<Guid>(d: GMap2): int
    return d.Count;
  end F;
end U;
public class Program
  public static method Main()
    var d: Dictionary<Guid, int> := new Dictionary<Guid, int>();
    d[Guid.NewGuid()] := 5;
    Console.WriteLine(U.F<int>(d));
  end Main;
end Program;
