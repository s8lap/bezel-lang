//! case: s9_2_0029
//! doc: spec
//! section: 9.2
//! title: a record's own-typed Equals fills the Object slot when a ref-mode Equals overload follows it
//! rule: "On every value kind the `Equals` is the one taking the kind's own type ... declared `override` or plain ...: the one member that fills the kind's `Object` slot" ([§9.2]); "overloads differing in any of these are distinguishable", and the `ref`/`out` modifiers are one of them ([§22.6]), so `Equals(ref other: R)` is an ordinary second overload.
//! cites: 9.2, 22.6, 8.3
//! expect: run
//! stdout: True
//! issue: 6263
module Conf;

public record R(X: int, Y: int)
  public method Equals(other: R): bool => (X mod 10) = (other.X mod 10);
  public method Equals(ref other: R): bool => false;
  public method GetHashCode(): int => (X mod 10) + 500;
end R;

public class Program
  public static method Main()
    var a := new R(5, 1);
    var b := new R(15, 2);
    var o: Object := a;
    Console.WriteLine(o.Equals(b).ToString());
  end Main;
end Program;
