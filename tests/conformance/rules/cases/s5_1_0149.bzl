//! case: s5_1_0149
//! doc: spec
//! section: 5.1
//! title: a nullable value does not implicitly convert to a non-nullable parameter
//! rule: spec §5.1: "For any type `T`, `T` implicitly converts to `T?` ...
//!     Each implicit conversion of this section also **lifts** to the nullable
//!     forms: `S?` implicitly converts to `T?` whenever `S` implicitly converts
//!     to `T`". The lifting runs one way only: there is no `S?` → `T`
//!     conversion, so an `int?` reaches no `int` parameter. The local is never
//!     narrowed — §7.4 narrows a declaration only from a non-nullable
//!     initializer, and this one is nullable-typed.
//! cites: 5.1, 7.4, 22.6
//! expect: reject
//! diagnostic: no matching overload
module Conf;

public class Program
  public static method Take(radix: int): string
    return "ok";
  end Take;

  public static method Radix(): int?
    return 16;
  end Radix;

  public static method Main()
    var r: int? := Radix();
    Console.WriteLine(Take(r));
  end Main;
end Program;
