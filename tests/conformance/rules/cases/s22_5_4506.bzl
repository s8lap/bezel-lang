//! case: s22_5_4506
//! doc: spec
//! section: 22.5
//! title: a nullable argument named to the non-nullable string parameter of int.Parse
//!   is rejected
//! rule: A non-nullable reference type implicitly converts to its nullable form. The
//!   reverse requires a null check or `!`.
//! cites: 22.5, 5.1
//! expect: reject
//! diagnostic: string
//! issue: 7602
module Conf;

class C
  public static method Maybe(): string?
    return null;
  end Maybe;
  public static method Main(): int
    var n := Maybe();
    Console.WriteLine(int.Parse(radix := 16, s := n));
    return 0;
  end Main;
end C;
