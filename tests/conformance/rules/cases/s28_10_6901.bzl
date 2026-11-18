//! case: s28_10_6901
//! doc: spec
//! section: 28.10
//! title: the spelled-out for-in expansion over an owned array local is rejected at the
//!   unmarked using var
//! rule: An unmarked `using var y := x;` is a **compile error** when its right-hand side is
//!   a named owned binding whose declared type, nullability stripped, is a class or
//!   interface type
//! cites: 28.10, 29.2, 28.3
//! expect: reject
//! diagnostic: does not say whether
module Conf;
public class W
  public static method Main(): int
    var source: Array<int> := [1, 2, 3];
    do
      using var __source := source;
      using var __enumerator := __source.GetEnumerator();
      while __enumerator.MoveNext() do
        var x := __enumerator.Current;
      end;
    end;
    return 0;
  end Main;
end W;
