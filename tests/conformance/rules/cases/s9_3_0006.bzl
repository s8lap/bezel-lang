//! case: s9_3_0006
//! doc: spec
//! section: 9.3
//! title: a positional record declaring plain Equals and GetHashCode keeps neither generated member
//! rule: a disqualifying body member (an `Equals`/`GetHashCode` override, a
//!   homogeneous `=`/`<>` overload, or any other member outside the §9.2
//!   list) suppresses the auto-generated equality members ... An `override`
//!   of `Equals` or `GetHashCode`, either spelling being the override
//! cites: 9.3, 9.2
//! expect: run
//! output: True
//!   False
//! issue: 5924
module Conf;

public record Parity(X: int)
  public method Equals(other: Parity): bool => X mod 2 = other.X mod 2;
  public method GetHashCode(): int => X mod 2;
end Parity;

public class Program
  public static method Main()
    Console.WriteLine(new Parity(1).Equals(new Parity(3)));
    Console.WriteLine(new Parity(1).Equals(new Parity(2)));
  end Main;
end Program;
