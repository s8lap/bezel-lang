//! case: s8_3_0075
//! doc: spec
//! section: 8.3
//! title: a plain Equals taking an interface type fills no Object slot, so a default on it is accepted
//! rule: "A default value is permitted only on parameters with no modifier or `owned`" ([§22.5]). The no-default restriction names the slot's parameter alone: "The slot's parameter declares no default value, on a class or a value kind" ([§8.3]). Here `Equals(other: I? := null)` takes the interface `I`, neither `Object?` nor the class's own type, and "Overriding `Equals` is filling the `Object` slot" only for those spellings ([§8.3]), so this member fills no slot and its default is legal.
//! cites: 8.3, 22.5
//! expect: run
//! output: accepted
module Conf;
public interface I
  method Equals(other: I? := null): bool;
end I;
public class C: I
  public method Equals(other: I? := null): bool => true;
end C;
public class Program
  public static method Main(): int
    Console.WriteLine("accepted");
    return 0;
  end Main;
end Program;
