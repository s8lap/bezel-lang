//! case: s7_4_0041
//! doc: spec
//! section: 7.4
//! title: reassigning a narrowed local from a nullable expression inside a nested if invalidates it
//! rule: The narrowing of a variable is invalidated when, within the controlled block:
//!   the variable is reassigned from a nullable-typed expression; or   (v0.338 §7.4 lines 906, 908)
//! cites: 12.7
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Probe(s: string?, n: string?, k: int)
    if s <> null then
      if k = 1 then s := n; end if;
      Console.WriteLine(s.Length);
    end if;
  end Probe;

  public static method Main()
    Probe("ab", null, 1);
  end Main;
end Program;
