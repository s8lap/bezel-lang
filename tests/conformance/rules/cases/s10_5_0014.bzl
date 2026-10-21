//! case: s10_5_0014
//! doc: spec
//! section: 10.5
//! title: an enum declaring its own-typed Equals without GetHashCode is rejected
//! rule: "A type that overrides `Equals` must also override `GetHashCode`. The compiler enforces this." ([§8.3]); on a value kind whose `Object`-slot `Equals` takes its own type and is declared plain, "a plain `Equals` obliges `GetHashCode`" ([§8.3]), and an enum fills that slot ([§10.5]).
//! cites: 8.3, 10.5
//! expect: reject
//! diagnostic: GetHashCode
module Conf;

enum E
  A, B;
  public method Equals(other: E): bool => true;
end E;

public class Program
  public static method Main()
    Console.WriteLine(E.A.Equals(E.B).ToString());
  end Main;
end Program;
