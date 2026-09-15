//! case: s6_6_0015
//! doc: spec
//! section: 6.6
//! title: the narrow-integer write-back does not extend to a plain assignment
//! rule: The write-back is scoped to the compound form only: plain b := b + 10
//!   remains the §6.2/§5.1 compile error it has always been, so the
//!   no-implicit-narrowing rule is undisturbed everywhere else.
//! cites: 6.6, 6.2, 5.1
//! expect: reject
//! diagnostic: implicit
module Conf;

public class Program
  public static method Main()
    var b: byte := 10;
    b := b + 10;
    Console.WriteLine(b);
  end Main;
end Program;
