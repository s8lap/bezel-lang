//! case: s17_5_0027
//! doc: spec
//! section: 17.5
//! title: a bool left operand does not implicitly convert to an Array<int> element
//! rule: "Between integers and `bool`" is never implicit ([§5.1]), so the
//!   left operand is not admissible as `Contains(item: int)`'s argument
//!   ([§17.5]).
//! cites: 17.5, 5.1
//! expect: reject
//! diagnostic: bool
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2];
    var t: bool := true;
    var found := t in a;
    Console.WriteLine(found);
  end Main;
end Program;
