//! case: s27_2_0003
//! doc: spec
//! section: 27.2
//! title: a non-null function returning a nullable value is a distinct type from a nullable function
//! rule: Without the parentheses, the `?` attaches to the return type —
//!   `(int) -> bool?` is a non-null function returning a nullable bool, a
//!   different type from `((int) -> bool)?`, and modifier/nullability slots
//!   follow the standard §7 rules under assignability (§27.1.1).
//! cites: 27.2, 27.1.1
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public method Demo()
    var nullableReturn: (int) -> bool? := lambda(x: int): bool? => x > 0;
    var nullableFunc: ((int) -> bool)? := nullableReturn;
  end Demo;
end Program;
