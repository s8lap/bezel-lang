//! case: s7_1_7201
//! doc: spec
//! section: 7.1
//! title: a lambda returning a nullable local is rejected at a non-nullable
//!   return slot
//! rule: implicitly converts to its nullable form. The reverse requires a
//!   null check or `!`.
//! cites: 7.1, 27.3
//! expect: reject
//! diagnostic: nullable
module Conf;

public class P
  public static method Mk(): string? => null;

  public static method Main()
    var k: string? := P.Mk();
    var f: () -> string := lambda() => k;
    Console.WriteLine(f());
  end Main;
end P;
