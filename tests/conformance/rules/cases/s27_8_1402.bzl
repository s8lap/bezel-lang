//! case: s27_8_1402
//! doc: spec
//! section: 27.8
//! title: an alias whose body names Guid is not parameter-typed: a Dictionary<int, int> argument is rejected
//! rule: A type alias is a pure synonym — it does not create a new type.
//! cites: 27.8, 5.1
//! expect: reject
//! diagnostic: implicitly convertible
module Conf;
type GMap := Dictionary<Guid, int>;
public class Box<Guid>
  public method Get(d: GMap): int
    return d.Count;
  end Get;
end Box;
public class Program
  public static method Main()
    var d: Dictionary<int, int> := [0: 7];
    var b := new Box<int>();
    Console.WriteLine(b.Get(d));
  end Main;
end Program;
