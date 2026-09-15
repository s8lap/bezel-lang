//! case: s18_6_0001
//! doc: spec
//! section: 18.6
//! title: a verbatim string suppresses escape processing — backslashes are literal
//! rule: "A `@` prefix suppresses escape processing." Example: `var path :=
//!   @"C:\Users\Bezel\Program.bzl";` — the backslashes are literal characters,
//!   not escape introducers.
//! cites: 18.6
//! expect: run
//! stdout: C:\Users\Bezel\Program.bzl
module Conf;

public class Program
  public static method Main()
    var path := @"C:\Users\Bezel\Program.bzl";
    Console.WriteLine(path);
  end Main;
end Program;
