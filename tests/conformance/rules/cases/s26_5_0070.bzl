//! case: s26_5_0070
//! doc: spec
//! section: 26.5
//! title: a bound reference to a name supplied by two equally specific extensions is ambiguous
//! rule: If two equally-specific extensions match, the call is a compile error.
//! cites: 26.5, 27.6
//! expect: reject
//! diagnostic: ambig
//! issue: 7488
module Conf;

extension Array<int>
  public method Length(): int => 1;
end;

extension Array<int>
  public method Length(): int => 2;
end;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var f: () -> int := a.Length;
    Console.WriteLine(f());
  end Main;
end Program;
