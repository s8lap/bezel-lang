//! case: s28_2_3605
//! doc: spec
//! section: 28.2
//! title: a span's Length read after its source's dominating disposal is rejected
//! rule: a use of the holder — a read or member access through it (a span's own components `Start`,
//!   `Length`, `IsEmpty` and `Slice` included: that they touch no storage, the checked-mode bullet above,
//!   is a statement about run time, not an exception to this rule) … — is the same compile error
//! cites: 28.2, 9.7
//! expect: reject
//! diagnostic: dominat
//! issue: 8748
//! notes: Accepting twin s28_2_3606 (the Length read moves before the kill).
module Conf;

public class Program
  public static method Run(): int
    var xs: Array<int> := [1, 2, 3];
    var s := xs.AsSpan();
    xs.Dispose();
    return s.Length;
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;
