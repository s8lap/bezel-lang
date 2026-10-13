// RUN: %bezelc -c -o %t.o %s
//
// spec §14.2, v0.338 / Appendix C.4, v0.338 / #5805 follow-up: the over-rejection
// guard for the extension-block Object rule. The remedies the rule names
// (`where T: notnull`, an unconstrained block parameter) and a user class bound
// `where T: Animal` (spec :2368, `where T: SomeClass`) and an interface bound
// `where T: IComparable<T>` (spec §14.2 interface row) must still compile.

module M;

public class Animal
  public constructor()
  end constructor;
end Animal;

public extension IEnumerable<T> where T: notnull
  public method Foo(): int => 1;
end;

public extension IEnumerable<T> where T: Animal
  public method Bar(): int => 2;
end;

public extension IEnumerable<T> where T: IComparable<T>
  public method Baz(): int => 3;
end;
