//! case: s19_8_0003
//! doc: spec
//! section: 19.8
//! title: a named slot's bare name reads the slot's enum, not the scrutinee's same-named case
//! rule: At a nested slot of a destructuring pattern, enum and choice alike, the
//!   omission is read against the slot's declared type, not the scrutinee's
//!   (§35.5 *Case patterns*).
//! cites: 19.8, 35.5
//! expect: run
//! output: set low
//!   cmd low
//!   set high
//! notes: `Low` is both a case of the scrutinee's choice `Cmd` and a constant of the
//!   slot's enum `Level`; inside `Set(L = Low)` the slot's type decides, while the
//!   top-level arm `Low` names the choice case.
module Conf;

public enum Level
  Low, High;
end Level;

public choice Cmd
  Set(L: Level),
  Low;
end Cmd;

public class Program
  public static method Describe(c: Cmd)
    case c of
      Set(L = Low)  => Console.WriteLine("set low");,
      Set(L = High) => Console.WriteLine("set high");,
      Low           => Console.WriteLine("cmd low");
    end case;
  end Describe;

  public static method Main()
    Describe(Cmd.Set(Level.Low));
    Describe(Cmd.Low);
    Describe(Cmd.Set(Level.High));
  end Main;
end Program;
