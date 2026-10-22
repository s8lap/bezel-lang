//! case: s8_3_0077
//! doc: spec
//! section: 8.3
//! title: a class's own-typed Equals with a default value is rejected at its declaration
//! rule: "The slot's parameter declares no default value, on a class or a value kind" ([§8.3]). "A class's `Equals` taking the class's own type, `C` or `C?`, is the `Object` slot's second spelling, virtual, written with or without `override`" (the same sentence's section). So `Equals(other: C? := null)` on `C` fills the slot and may not declare the default; `C` declares `GetHashCode` beside it, so the default is the only violation.
//! cites: 8.3, 22.5
//! expect: reject
//! diagnostic: takes no default value
module Conf;
public interface I
  method Equals(other: C? := null): bool;
end I;
public class C: I
  public method Equals(other: C? := null): bool => true;
  public override method GetHashCode(): int => 0;
end C;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
