//! case: s17_5_0028
//! doc: spec
//! section: 17.5
//! title: a bool left operand is admitted into an Array<bool> and finds its element
//! rule: "**identity**: every type converts to itself" ([§5.1]), so the `bool` left operand is admissible as `Contains(item: bool)`'s argument ([§17.5]); a primitive's default `Equals` is value comparison ([§8.3]), so the element is found.
//! cites: 17.5, 5.1, 8.3
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var a: Array<bool> := [true];
    var t: bool := true;
    var found := t in a;
    Console.WriteLine(found);
  end Main;
end Program;
