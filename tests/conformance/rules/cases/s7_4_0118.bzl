//! case: s7_4_0118
//! doc: spec
//! section: 7.4
//! title: a call result is not a binding the compiler tracks a narrowing for
//! rule: spec §7.4: "at each program point the compiler tracks, per local
//!     variable and per parameter other than a `ref` parameter ..., whether its
//!     value is known non-null", and the establishing forms it lists are a null
//!     test, a `case` arm, boolean composition, an early exit, an assignment,
//!     a declaration's initializer, and a `when true` postcondition. A call's
//!     result is none of them, so the nullable it returns stays nullable.
//! cites: 7.4, 5.1
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
    Console.WriteLine(Take(Radix()));
  end Main;
end Program;
