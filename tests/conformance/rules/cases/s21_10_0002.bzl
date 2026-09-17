//! case: s21_10_0002
//! doc: spec
//! section: 21.10
//! title: a local record may not carry a visibility keyword
//! rule: The `Visibility?` slot must be empty — a local type has no access
//!   surface, so a visibility keyword is a compile error naming the rule.
//! cites: 21.10
//! expect: reject
//! diagnostic: visibility
module Conf;

public class Program
  public static method Main()
    public record Tagged(Name: string);
    var t := new Tagged("x");
    Console.WriteLine(t.Name);
  end Main;
end Program;
