//! case: s7_7_0023
//! doc: spec
//! section: 7.7
//! title: ??= over a string? binding narrowed non-null by §7.4 draws no warning
//! rule: "Where the left operand is a read of a binding, the binding's declared type decides, so a nullable binding §7.4 has narrowed non-null draws nothing (`var s: string? := "a"; s ??= "b";` is silent)"
//! cites: 7.7, 7.4, 6.6
//! expect: quiet
//! issue: 8596
module Conf;

public class Program
  public static method Fill(): string
    var s: string? := "a";
    s ??= "b";
    return s;
  end Fill;
end Program;
