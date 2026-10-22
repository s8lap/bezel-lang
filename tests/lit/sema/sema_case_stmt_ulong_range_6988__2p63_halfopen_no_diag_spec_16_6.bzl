// #6988 (§16.6, §5.1, v0.340): a half-open statement-form range over a ulong
// whose bounds lie at or above 2^63 (`2^63..<2^64-1`) must compile with no
// diagnostic. Both bounds fold to negative long long values.
//
// Non-vacuity: the first RUN line requires the program to compile (exit 0).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
public class Program
  public static method Main()
    var u: ulong := 9223372036854775808UL;
    case u of
      9223372036854775808UL..<18446744073709551615UL => Console.WriteLine("in"),
      _ => Console.WriteLine("out")
    end case;
  end Main;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: §
