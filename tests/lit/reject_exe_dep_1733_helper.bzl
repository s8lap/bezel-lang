// RUN: %bezelc -c -o %t.o %s
//
// Helper artifact for reject_exe_dep_1733.bzl (issue #1733 / spec §2.8.4 rule 4,
// v0.188). Declares `module A;` with a public `Greeter` the sibling test
// imports, plus a qualifying `Main` (§33.2) so that building this file WITHOUT
// `-c` (`%bezelc -o …`) produces an EXECUTABLE-kind artifact — the thing the
// sibling test presents as a dependency and expects to be rejected. Built WITH
// `-c` it is an ordinary LIBRARY (the `Main` is inert here, §33.6/§2.8.4 rule 1)
// — the sibling test's positive control that the SAME interface imports fine as
// a library. This file's own RUN line is that library accept (a zero exit is
// the pass).
module A;

public class Greeter
  public static method Greet(): int return 42; end Greet;
end Greeter;

class Program
  public static method Main(): int return 0; end Main;
end Program;
