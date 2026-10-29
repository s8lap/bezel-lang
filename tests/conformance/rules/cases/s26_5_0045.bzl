//! case: s26_5_0045
//! doc: spec
//! section: 26.5
//! title: an extension on string supplies CountOf and GetField, which string lacks, at several arities
//! rule: If the receiver's type declares no instance member of that name, the compiler searches the current module and all imported modules
//! cites: 26.5
//! expect: run
//! output: 42
//!   EXT3
//!   X
module Conf;

extension string
  public method CountOf(sub: string): int => 42;
  public method GetField(a: int, b: int, c: int): string => "EXT3";
  public method GetField(a: int, b: int): string => "X";
end;

public class Program
  public static method Main()
    Console.WriteLine("a,b".CountOf(","));
    Console.WriteLine("a,b".GetField(1, 2, 3));
    Console.WriteLine("abc".GetField(0, 1));
  end Main;
end Program;
