//! case: s7_4_0094
//! doc: spec
//! section: 7.4
//! title: a null test on a var local does not reach a lambda formed under it
//! rule: A lambda body (§27.5) reads the variables it captures at their declared
//!   types, with one exception: a fact no form can invalidate after the
//!   lambda's formation enters the body as it stands there.
//! cites: 7.4, 27.5
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Make(): string? => "four";

  public static method Main()
    var s: string? := Make();
    if s <> null then
      var fn := lambda() => s.Length;
      Console.WriteLine(fn());
    end;
  end Main;
end Program;
