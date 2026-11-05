//! case: l2_2_2004
//! doc: stdlib
//! section: 2.2
//! title: Math.Clamp over Guid with named arguments in reverse order binds
//!   by parameter name
//! rule: `Clamp(value, low, high)` returns `low` when `value` compares less
//!   than `low`, `high` when it compares greater than `high`, and
//!   `value` otherwise.
//! cites: 2.6, 22.5
//! expect: run
//! output: 33333333-3333-3333-3333-333333333333
//! notes: stdlib 2.6: Guid CompareTo is lexicographic over the 16 canonical
//!   big-endian bytes (the textual hex order), unsigned. Failed on the
//!   compiler before the #7935 fix and passes on the staging tip.
module Conf;

public class Program
  public static method Main()
    var lo := Guid.Parse("11111111-1111-1111-1111-111111111111");
    var hi := Guid.Parse("33333333-3333-3333-3333-333333333333");
    var y := Guid.Parse("80000000-0000-0000-0000-000000000000");
    Console.WriteLine(Math.Clamp(high := hi, low := lo, value := y).ToString());
  end Main;
end Program;
