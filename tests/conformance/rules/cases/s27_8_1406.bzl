//! case: s27_8_1406
//! doc: spec
//! section: 27.8
//! title: an alias of an alias keeps its body's Guid: a Dictionary<int, int> argument is rejected
//! rule: A type alias is a pure synonym — it does not create a new type.
//! cites: 27.8, 5.1
//! expect: reject
//! diagnostic: implicitly convertible
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
    var d: Dictionary<int, int> := [0: 7];
    Console.WriteLine(U.F<int>(d));
  end Main;
end Program;
