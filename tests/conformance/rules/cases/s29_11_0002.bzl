//! case: s29_11_0002
//! doc: spec
//! section: 29.11
//! title: a lazy query enumerated a second time is a compile error
//! rule: A lazy query ... is single-consumption. It may be enumerated at
//!   most once ... A second enumeration, or any other use after the first,
//!   is a compile error where the flow analysis can see it statically (the
//!   diagnostic points to `.ToArray()`).
//! cites: 29.11
//! expect: reject
//! diagnostic: ToArray
module Conf;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    using var q := xs.Where(lambda(x) => x > 1);
    for var v in q do
      _ := v;
    end;
    for var w in q do
      _ := w;
    end;
  end Main;
end Program;
