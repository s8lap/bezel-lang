//! case: s26_5_2001
//! doc: spec
//! section: 26.5
//! title: two equally specific Object extensions of the same zero-argument
//!   Select make the call ambiguous
//! rule: If two equally-specific extensions match, the call is a compile
//!   error.
//! cites: 26.5.1
//! expect: reject
//! diagnostic: ambiguous
module Conf;

extension Object
  public method Select(): int => 1;
end;

extension Object
  public method Select(): int => 2;
end;

public class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(1);
    Console.WriteLine(a.Where(lambda(x) => x > 0).Select());
  end Main;
end Program;
