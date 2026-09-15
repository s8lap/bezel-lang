//! case: s26_7_0005
//! doc: spec
//! section: 26.7
//! title: a value-kind extension body may read the receiver's own storage
//! rule: For a value-kind receiver, §9.4's value-receiver rule applies in
//!   the extension body exactly as in a declared member body — only a
//!   *store* landing in the receiver's own storage is rejected; a bare
//!   read of the same storage is ordinary member access and is legal.
//! cites: 26.7, 9.4
//! expect: run
//! stdout: 6
module Conf;

public record Point
  public property X: int;

  public constructor(x: int)
    X := x;
  end constructor;
end Point;

public extension Point
  public method Bump(): int => X + 1;
end;

public class Program
  public static method Main()
    var p := new Point(5);
    Console.WriteLine(p.Bump());
  end Main;
end Program;
