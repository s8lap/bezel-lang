//! case: s30_14_4601
//! doc: spec
//! section: 30.14
//! title: a Domain.Spawn entry returning a string crosses as a copy and the join yields it
//! rule: A sendable value crosses as a copy.
//! cites: stdlib 6.8
//! expect: run
//! stdout: hello
module Conf;

import System.Threading;

public class Program
  public static method Make(): string => "hello";
  public static async method Main(): Task
    using var t := Domain.Spawn(Program.Make);
    var s := await t;
    Console.WriteLine(s);
  end Main;
end Program;
