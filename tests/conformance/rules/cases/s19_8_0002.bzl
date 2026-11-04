//! case: s19_8_0002
//! doc: spec
//! section: 19.8
//! title: at a named slot of enum type the prefix is omitted though the scrutinee is a record
//! rule: At a nested slot of a destructuring pattern, enum and choice alike, the
//!   omission is read against the slot's declared type, not the scrutinee's
//!   (§35.5 *Case patterns*).
//! cites: 19.8, 19.3, 35.5
//! expect: run
//! output: a high
//!   b other
//! notes: the scrutinee `Job` is a record, so only the slot's type `Priority` can
//!   resolve the bare `High`.
module Conf;

public enum Priority
  Low, High;
end Priority;

public record Job(P: Priority, Name: string);

public class Program
  public static method Report(j: Job)
    case j of
      Job(P = High) => Console.WriteLine($"{j.Name} high");,
      _             => Console.WriteLine($"{j.Name} other");
    end case;
  end Report;

  public static method Main()
    Report(new Job(Priority.High, "a"));
    Report(new Job(Priority.Low, "b"));
  end Main;
end Program;
