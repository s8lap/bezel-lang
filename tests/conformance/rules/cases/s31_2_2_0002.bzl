//! case: s31_2_2_0002
//! doc: spec
//! section: 31.2.2
//! title: a single-statement catch arm body ends with the statement's semicolon
//! rule: A catch arm's body may be a single statement: `=> Log(ex);` ends
//!   with the statement's `;` — the convenient form for a one-line arm body
//!   that is a statement.
//! cites: 31.2.2
//! expect: run
//! stdout: handled
module Conf;

public class BoomException: Exception
  public constructor(message: string) base(message)
  end constructor;
end BoomException;

public class Program
  public static method Main()
    try
      throw new BoomException("x");
    catch
      BoomException => Console.WriteLine("handled");
    end try;
  end Main;
end Program;
