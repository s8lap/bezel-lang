// RUN: %bezelc --link=system -o %t.exe %s && %t.exe | FileCheck %s
// RUN: %bezelc --help | FileCheck %s --check-prefix=HELP
// #6654: --link=system selects the hosted (clang) link explicitly, and --help
// documents both modes.
// CHECK: linked by clang
// HELP: [--link=self-contained|system]
// HELP: --link=self-contained
// HELP: --link=system
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("linked by clang");
  end Main;
end Program;
