//! case: s15_3_0009
//! doc: spec
//! section: 15.3
//! title: assigning a fresh owned value restores a slot moved-from via move arr[i]
//! rule: The standard restoration mechanisms apply: assign a fresh owned value
//!   to the slot moved-from via `move arr[i]` before any exit edge (§15.3,
//!   §15.1.1).
//! cites: 15.3, 15.1.1
//! expect: run
//! output: first
//!   second
module Conf;

public class Widget
  public property Tag: string;

  public constructor(tag: string)
    Tag := tag;
  end constructor;
end Widget;

public class Program
  public static method Main()
    var a: [1 owned]Widget := [new Widget("first")];
    using var taken := move a[0];
    a[0] := new Widget("second");
    Console.WriteLine(taken.Tag);
    Console.WriteLine(a[0].Tag);
  end Main;
end Program;
