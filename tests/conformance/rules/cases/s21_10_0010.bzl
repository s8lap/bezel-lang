//! case: s21_10_0010
//! doc: spec
//! section: 21.10
//! title: a local type may not shadow a type visible at its declaration point
//! rule: The declared name may not shadow any type name visible at the
//!   declaration point (a local `NameAge` silently shadowing a module-level
//!   `NameAge` is a trap, not a convenience — the collision is an error).
//! cites: 21.10, C.1
//! expect: reject
//! diagnostic: shadow
module Conf;

public record Row(Name: string);

public class Program
  public static method Main()
    record Row(Name: string);
    var r := new Row("local");
    Console.WriteLine(r.Name);
  end Main;
end Program;
