//! case: s9_2_0031
//! doc: spec
//! section: 9.2
//! title: an IEquatable<R> call on a record reaches the record's own-typed Equals
//! rule: "An interface member at that signature — and one at `GetHashCode(): int` or `ToString(): string` — is satisfied by the kind's own member for that slot, declared or generated" ([§9.2], [§14.2]).
//! cites: 9.2, 14.2
//! expect: run
//! stdout: True
module Conf;

public record R(X: int, Y: int): IEquatable<R>
  public method Equals(other: R): bool => (X mod 10) = (other.X mod 10);
  public method GetHashCode(): int => (X mod 10) + 500;
end R;

public class Program
  public static method Via(a: IEquatable<R>, b: R): bool => a.Equals(b);

  public static method Main()
    var a := new R(5, 1);
    var b := new R(15, 2);
    Console.WriteLine(Via(a, b).ToString());
  end Main;
end Program;
