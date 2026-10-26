//! case: s5_1_0150
//! doc: spec
//! section: 5.1
//! title: a nullable value lifts to the nullable form of the target
//! rule: spec §5.1: "Each implicit conversion of this section also **lifts** to
//!     the nullable forms: `S?` implicitly converts to `T?` whenever `S`
//!     implicitly converts to `T` — a present value converts by the underlying
//!     conversion, `null` converts to `null` (`int?` → `long?`)". The `int?`
//!     argument reaches a `long?` parameter. The local is never narrowed — §7.4
//!     narrows a declaration only from a non-nullable initializer — so the
//!     argument's static type is `int?` and the lifting is what admits it.
//! cites: 5.1, 7.4
//! expect: run
//! stdout: ok
module Conf;

public class Program
  public static method Take(radix: long?): string
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
