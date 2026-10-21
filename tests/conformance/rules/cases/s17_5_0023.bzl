//! case: s17_5_0023
//! doc: spec
//! section: 17.5
//! title: a double left operand does not implicitly convert to a Set<int> element
//! rule: "A value of one type implicitly converts to another when no programmer-visible information is lost" ([§5.1]); the §5.1 table has no `double` row, so the `double` left operand is not admissible as `Contains(item: int)`'s argument ([§17.5]).
//! cites: 17.5, 5.1
//! expect: reject
//! diagnostic: implicitly convert
module Conf;

public class Program
  public static method Main()
    var s: Set<int> := [1, 2];
    var d: double := 1.5;
    var found := d in s;
    Console.WriteLine(found);
  end Main;
end Program;
