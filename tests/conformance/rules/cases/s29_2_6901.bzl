//! case: s29_2_6901
//! doc: spec
//! section: 29.2
//! title: the spelled-out for-in expansion over a borrowed local compiles and runs: the
//!   adoption is a no-op
//! rule: When `source` is a value-type expression, a runtime-managed expression (e.g. a
//!   string), or a borrowed local of class type, the `using var __source := source;` is a
//!   no-op
//! cites: 29.2, 28.10
//! expect: run
//! stdout: 6
module Conf;
public class W
  public static method Main(): int
    var sourceOwner: Array<int> := [1, 2, 3];
    var source := sourceOwner;
    var total := 0;
    do
      using var __source := source;
      using var __enumerator := __source.GetEnumerator();
      while __enumerator.MoveNext() do
        var x := __enumerator.Current;
        total := total + x;
      end;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end W;
