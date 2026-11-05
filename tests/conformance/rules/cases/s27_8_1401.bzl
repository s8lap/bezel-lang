//! case: s27_8_1401
//! doc: spec
//! section: 27.8
//! title: an alias whose body names Guid denotes the module's Guid inside a class with a type parameter Guid
//! rule: A type alias is a pure synonym — it does not create a new type.
//! cites: 27.8, 2.7
//! expect: run
//! stdout: 1
module Conf;
type GMap := Dictionary<Guid, int>;
public class Box<Guid>
  public method Get(d: GMap): int
    return d.Count;
  end Get;
end Box;
public class Program
  public static method Main()
    var d: Dictionary<Guid, int> := new Dictionary<Guid, int>();
    d[Guid.NewGuid()] := 5;
    var b := new Box<int>();
    Console.WriteLine(b.Get(d));
  end Main;
end Program;
