//! case: s22_5_0005
//! doc: spec
//! section: 22.5
//! title: a default value may be a literal or constant expression
//! rule: "A default-value expression must be type-compatible with the
//!   parameter's declared type ... [it] may be a literal, constant, or
//!   `default(T)`."
//! cites: 22.5
//! expect: run
//! stdout: 4096
module Conf;

public class Buffer
  public static method Describe(size: int := 4096): int => size;
end Buffer;

public class Program
  public static method Main()
    Console.WriteLine(Buffer.Describe());
  end Main;
end Program;
