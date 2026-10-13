//! case: s15_3_0008
//! doc: spec
//! section: 15.3
//! title: a non-nullable inline-array slot taken by a bare move arr[i] is rejected
//! rule: A slot taken by `move arr[i]` is exchanged, never emptied (§28.11;
//!   the replacing `move`, §28.8.1): a `[N owned]T?` slot holds `null`
//!   afterwards and a `[N owned]T` slot holds its replacement
//! cites: 15.3, 15.1.1, 28.8.1
//! expect: reject
//! diagnostic: reassigned
module Conf;

public class Widget
  public property Tag: string;

  public constructor(tag: string)
    Tag := tag;
  end constructor;
end Widget;

public class Program
  public static method Run()
    var a: [1 owned]Widget := [new Widget("first")];
    using var taken := move a[0];
    Console.WriteLine(taken.Tag);
  end Run;
end Program;
