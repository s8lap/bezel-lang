// RUN: %not %bezelc --link=static -o %t.exe %s 2>&1 | FileCheck %s
// RUN: %not %bezelc --link= -o %t.exe %s 2>&1 | FileCheck %s --check-prefix=EMPTY
// #6654: --link takes exactly `self-contained` or `system`; anything else is a
// usage error (rc 2) naming the value.
// CHECK: bezelc: error: unknown link mode 'static' (expected 'self-contained' or 'system')
// EMPTY: bezelc: error: unknown link mode '' (expected 'self-contained' or 'system')
module Conf;

public class Program
  public static method Main()
  end Main;
end Program;
