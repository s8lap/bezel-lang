//! case: s22_4_0008
//! doc: spec
//! section: 22.4
//! title: reading a non-nullable-reference out variable off the guarded path is rejected
//! rule: On a path where the call may not have run, a read of a
//!   **non-nullable-reference-typed** out variable is a compile error,
//!   exactly as for any other unassigned non-nullable local — an inline
//!   `out` variable counts as assigned only on paths where the introducing
//!   call was actually evaluated.
//! cites: 22.4, 7.9
//! expect: reject
//! diagnostic: definitely assigned
module Conf;

public class Parser
  public static method TryLabel(input: string, out label: string): bool
    if input = "42" then
      label := "forty-two";
      return true;
    end if;
    label := "";
    return false;
  end TryLabel;
end Parser;

public class Program
  public static method Main()
    var flag := true;
    if flag and Parser.TryLabel("42", out var label) then
    end if;
    Console.WriteLine(label);
  end Main;
end Program;
