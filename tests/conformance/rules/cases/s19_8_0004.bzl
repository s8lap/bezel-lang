//! case: s19_8_0004
//! doc: spec
//! section: 19.8
//! title: a named slot's bare name naming only a case of the scrutinee's choice is rejected
//! rule: At a nested slot of a destructuring pattern, enum and choice alike, the
//!   omission is read against the slot's declared type, not the scrutinee's
//!   (§35.5 *Case patterns*).
//! cites: 19.8, 19.3, 35.5
//! expect: reject
//! diagnostic: Stop
//! notes: `Stop` is a case of the scrutinee's choice `Cmd` but neither a constant of
//!   the slot's enum `Level` nor a type, so the slot head resolves to nothing.
//!   Valid twin: `Set(L = Low)`, as in s19_8_0003.
module Conf;

public enum Level
  Low, High;
end Level;

public choice Cmd
  Set(L: Level),
  Stop;
end Cmd;

public class Program
  public static method Describe(c: Cmd)
    case c of
      Set(L = Stop) => Console.WriteLine("set stop");,
      _             => Console.WriteLine("other");
    end case;
  end Describe;

  public static method Main()
    Describe(Cmd.Stop);
  end Main;
end Program;
