//! case: l6_8_0002
//! doc: stdlib
//! section: 6.8
//! title: Spawn's class-typed result transfers ownership to the awaiter
//! rule: Class-typed result: ownership transfers to the awaiter (Pattern
//!   B). Spawn<R>(entry: () -> owned R): owned OwningTask<R> where R:
//!   sendable, class;
//! cites: 6.8, 30.14
//! expect: run
//! stdout: 9
module Conf;

import System.Threading;

public sealed class Box
  public Value: int;
  public constructor(value: int) Value := value; end constructor;
end Box;

public class Program
  public static method MakeBox(): owned Box => new Box(9);

  public static async method Main(): Task
    using var t := Domain.Spawn(Program.MakeBox);
    using var b := await t;
    Console.WriteLine(b.Value);
  end Main;
end Program;
