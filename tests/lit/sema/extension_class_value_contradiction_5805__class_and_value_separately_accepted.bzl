// RUN: %bezelc -c -o %t.o %s
//
// spec §14.2, v0.338 / #5805 follow-up: the over-rejection guard for the
// extension class/value rule. `class` alone and `value` alone on an extension
// block's parameter are each admitted (§14.2), and a class bound beside a
// different parameter's `value` bound is not a contradiction, since the rule
// keys on one type parameter. None may be rejected.

module M;

public class Animal
  public constructor()
  end constructor;
end Animal;

public extension IEnumerable<T> where T: class
  public method Foo(): int => 1;
end;

public extension IEnumerable<T> where T: value
  public method Bar(): int => 2;
end;

public extension IEnumerable<T>
  public method Baz<U>(x: U): int where U: value => 3;
end;
