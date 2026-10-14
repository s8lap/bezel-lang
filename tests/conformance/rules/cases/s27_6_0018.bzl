//! case: s27_6_0018
//! doc: spec
//! section: 27.6
//! title: a generic reference whose type argument fails the callee's constraint is rejected at formation
//! rule: The formation is an instantiation site: its constraints are read at the formation exactly as at a call with that list (the constraint of Math.Min is where T: IComparable<T>).
//! cites: 27.6, 14.2
//! expect: reject
//! diagnostic: IComparable
//! issue: 5814
module Conf;

public record Tag(V: int);

public class Program
  public static method Main()
    var m: (Tag, Tag) -> Tag := Math.Min<Tag>;
    Console.WriteLine(1);
  end Main;
end Program;
