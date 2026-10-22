//! case: s7_4_0095
//! doc: spec
//! section: 7.4
//! title: a const copy formed inside the null test carries its narrowing into a lambda body
//! rule: A lambda body (§27.5) reads the variables it captures at their declared
//!   types, with one exception: a fact no form can invalidate after the
//!   lambda's formation enters the body as it stands there — a const local's
//! cites: 7.4, 27.5
//! expect: run
//! stdout: 4
module Conf;

public class Program
  public static method Make(): string? => "four";

  public static method Main()
    var s: string? := Make();
    if s <> null then
      const t := s;
      var fn := lambda() => t.Length;
      Console.WriteLine(fn());
    end;
  end Main;
end Program;
