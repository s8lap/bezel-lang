// RUN: %not %bezelc --link=self-contained -o %t.exe %s 2>&1 | FileCheck %s
// RUN: %bezelc --link=self-contained -c -o %t.o %s
// #6654: an explicit --link=self-contained in a build with no toolchain bundle
// next to bezelc (the dev build) is a usage error (rc 2) naming the directory
// it looked in and the way out; it never falls back to clang silently. `-c`
// performs no link, so there it is accepted and inert.
// CHECK: bezelc: error: --link=self-contained: no self-contained toolchain at '{{.*}}/lib/bezel/x86_64-linux-musl' (a release tarball ships it in lib/bezel/x86_64-linux-musl); pass --link=system to link with clang
module Conf;

public class Program
  public static method Main()
  end Main;
end Program;
