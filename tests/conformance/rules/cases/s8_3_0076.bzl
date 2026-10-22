//! case: s8_3_0076
//! doc: spec
//! section: 8.3
//! title: a plain Equals at an unrelated class's parameter type fills no Object slot, so a default is accepted
//! rule: "The slot's parameter declares no default value, on a class or a value kind" ([§8.3]) names the slot's parameter only. On a class the slot is "`override Equals(other: Object?)` or the own-typed spelling" ([§8.3]). `Equals(other: B? := null)` on `C` takes the unrelated class `B`, so it fills no slot, and "A default value is permitted only on parameters with no modifier or `owned`" ([§22.5]) admits the default.
//! cites: 8.3, 22.5
//! expect: run
//! output: accepted
module Conf;
public class B
end B;
public class C
  public method Equals(other: B? := null): bool => true;
end C;
public class Program
  public static method Main(): int
    Console.WriteLine("accepted");
    return 0;
  end Main;
end Program;
