//! case: s10_5_0015
//! doc: spec
//! section: 10.5
//! title: an enum with its own-typed Equals and no homogeneous = has no = at its static type
//! rule: "A declared `Equals` also retires the intrinsic `=`: an enum that declares its own-typed `Equals` and no homogeneous `=`/`<>` overload has no `=` or `<>` at all — a use at the enum's static type is a compile error at the expression" ([§10.5]).
//! cites: 10.5
//! expect: reject
//! diagnostic: equality
//! issue: 6602
module Conf;

enum E
  A, B;
  public method Equals(other: E): bool => true;
  public method GetHashCode(): int => 7;
end E;

public class Program
  public static method Main()
    Console.WriteLine((E.A = E.A).ToString());
  end Main;
end Program;
