//! case: s7_4_0120
//! doc: spec
//! section: 7.4
//! title: a narrowed int? passed as an int argument is its value, so IndexOf finds it
//! rule: After `if x is null then return; end;` "the surviving path is narrowed";
//!   the stdlib's `Array<T>.IndexOf(item: T, startIndex: int := 0): int` returns
//!   "first occurrence at or after startIndex, or -1". The narrowed `n` is a
//!   non-nullable operand, and `n` is 1, at index 0 of `[1, 2]`.
//! cites: 7.1, 7.4
//! expect: run
//! stdout: 0
module Conf;

public class C
  public static method Main(): int
    var n: int? := 1;
    if n is null then return 1; end;
    using var a: Array<int> := new Array<int>();
    a.Add(1); a.Add(2);
    Console.WriteLine(a.IndexOf(n).ToString());
    return 0;
  end Main;
end C;
