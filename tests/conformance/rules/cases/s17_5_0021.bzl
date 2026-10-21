//! case: s17_5_0021
//! doc: spec
//! section: 17.5
//! title: a non-constant int left operand does not implicitly convert to an Array<uint> element
//! rule: "Between signed and unsigned types of the same or narrower width (e.g. `int → uint`, `uint → int`, `short → ushort`)" is never implicit ([§5.1]); "This bullet governs **non-constant** expressions", so the `int` variable is not admissible as `Contains(item: uint)`'s argument ([§17.5]).
//! cites: 17.5, 5.1
//! expect: reject
//! diagnostic: signed and unsigned
module Conf;

public class Program
  public static method Main()
    var a: Array<uint> := [1, 2];
    var i: int := 1;
    var found := i in a;
    Console.WriteLine(found);
  end Main;
end Program;
