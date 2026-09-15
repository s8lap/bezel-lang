//! case: s35_12_0003
//! doc: spec
//! section: 35.12
//! title: an owning-choice field of a transfer-closed class is admitted when the choice holds no borrows
//! rule: §30.14's transfer-closure rule 2 admits an owning-choice-typed field of a
//!   transfer-closed class when the choice holds no borrows and every payload
//!   field of every case is transfer-compatible.
//! cites: 35.12, 30.14
//! expect: run
//! stdout: true
module Conf;

public sealed class Widget
  public constructor()
  end constructor;
end Widget;

public choice Outcome
  Fetched(owned w: Widget),
  Failed;
end Outcome;

public sealed class Envelope
  private owned _outcome: Outcome;

  public constructor(owned outcome: Outcome)
    _outcome := outcome;
  end constructor;
end Envelope;

public class Checker<T> where T: sendable
  public static method Check(): bool => true;
end Checker;

public class Program
  public static method Main()
    Console.WriteLine(Checker<Envelope>.Check());
  end Main;
end Program;
