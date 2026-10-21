// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// fix #6797 control (spec §7.3, stdlib §2.13, v0.340). `null` is a valid value
// of `Type?`, so this compiles with no diagnostic. Only a non-nullable `Type`
// target refuses `null`, and it keeps the spec §7 wording.
module M;
public class Program
  public static method Main(): int
    var t: Type? := null;
    return 0;
  end Main;
end Program;
// NODIAG-NOT: error
// NODIAG-NOT: warning
