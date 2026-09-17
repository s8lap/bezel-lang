//! case: s15_3_0006
//! doc: spec
//! section: 15.3
//! title: moving a move-only local while a closure capture of it is live is rejected
//! rule: The consume-site freeze applies regardless: moving a move-only local
//!   while a live borrow of it exists — e.g. a closure capture — is a compile
//!   error (§15.3, §28.9).
//! cites: 15.3, 28.9
//! expect: reject
//! diagnostic: borrow
module Conf;

public class Widget
  public property Tag: string;

  public constructor(tag: string)
    Tag := tag;
  end constructor;
end Widget;

public class Program
  public static method Run()
    var a: [1 owned]Widget := [new Widget("x")];
    var peek := lambda() => a[0].Tag;
    var b := a;
    Console.WriteLine(peek());
    Console.WriteLine(b[0].Tag);
  end Run;
end Program;
