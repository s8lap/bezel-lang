// RUN: %not %bezelc --link=self-contained --sanitize=address -o %t.exe %s 2>&1 | FileCheck %s
// RUN: %not %bezelc --sanitize=address --link=self-contained -o %t.exe %s 2>&1 | FileCheck %s
// #6654: AddressSanitizer has no musl runtime, so --sanitize=address under the
// self-contained link is a usage error (rc 2) that names the way out. It is
// checked before the bundle lookup, so it reads the same in a build that has
// no bundle.
// CHECK: bezelc: error: --sanitize=address is not supported by the self-contained (musl) link; pass --link=system to link with clang and the ASan runtime
module Conf;

public class Program
  public static method Main()
  end Main;
end Program;
