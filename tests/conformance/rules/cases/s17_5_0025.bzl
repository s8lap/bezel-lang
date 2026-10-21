//! case: s17_5_0025
//! doc: spec
//! section: 17.5
//! title: an int left operand does not implicitly convert to a char element
//! rule: The §5.1 table gives `char` implicit conversions to `int`, `uint`, `long`, `ulong`, `float`, `double` and `decimal`, and no row takes `int` to `char`; "A value of one type implicitly converts to another when no programmer-visible information is lost" ([§5.1]), so the left operand is not admissible as `Contains(item: char)`'s argument ([§17.5]).
//! cites: 17.5, 5.1
//! expect: reject
//! diagnostic: implicitly convert
module Conf;

public class Program
  public static method Main()
    var a: Array<char> := ['a'];
    var i: int := 97;
    var found := i in a;
    Console.WriteLine(found);
  end Main;
end Program;
