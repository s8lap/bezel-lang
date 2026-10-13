//! case: s17_11_0022
//! doc: spec
//! section: 17.11
//! title: an unconstrained default(T) is null at a nullable substitution and zero at int
//! rule: so at every admitted substitution the result is that type's zero
//!   value, and `null` exactly at a nullable substitution, where `null` is a
//!   value of `T`.
//! cites: 17.11, 18.5
//! expect: run
//! output: null
//!   0
module Conf;

public class Zero
  public static method Show<T>(): string
    var d: T := default(T);
    return $"{d}";
  end Show;

  public static method Main()
    Console.WriteLine(Zero.Show<int?>());
    Console.WriteLine(Zero.Show<int>());
  end Main;
end Zero;
