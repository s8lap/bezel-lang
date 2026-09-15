//! case: s21_9_0002
//! doc: spec
//! section: 21.9
//! title: the discard statement suppresses the unused-result warning
//! rule: To suppress the warning, use the discard form `_ := expression;`.
//! cites: 21.9, 17.13
//! expect: run
//! stdout: done
module Conf;

public class Program
  public static method ComputeChecksum(): int => 42;

  public static method Main()
    _ := ComputeChecksum();
    Console.WriteLine("done");
  end Main;
end Program;
