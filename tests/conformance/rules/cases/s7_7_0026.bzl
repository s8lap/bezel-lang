//! case: s7_7_0026
//! doc: spec
//! section: 7.7
//! title: ??= over a non-nullable int target with a converting right operand compiles and warns
//! rule: "A non-nullable left operand of type `T` types exactly as a left operand of type `T?` would: the right operand must still be implicitly convertible to `T` or to `T?`, and the result type is `T`, the left never being null"; "`x ??= e` over a non-nullable target (§6.6)" draws a warning "that the assignment never happens".
//!   Accepting twin of s7_7_0021: the right operand is an `int`, the one difference.
//! cites: 7.7, 6.6
//! expect: warn
//! diagnostic: never
//! issue: 8596
module Conf;

public class Program
  public static method Main()
    var i: int := 1;
    i ??= 2;
    Console.WriteLine(i);
  end Main;
end Program;
