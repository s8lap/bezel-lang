//! case: l2_6_0010
//! doc: stdlib
//! section: 2.6
//! title: Guid.TryParse accepts the D and N forms and assigns the zero Guid on failure
//! rule: Guid.TryParse accepts the D and N renderings and fails otherwise (stdlib 2.6); the out parameter is a non-nullable value-typed out, so failure assigns default(Guid), the all-zero value, never the previous contents (stdlib 1 shape (c), spec 22.3).
//! cites: 2.6, 1, 22.3
//! expect: run
//! output: valid-D True 11111111-2222-3333-4444-555555555555
//!   valid-N True 11111111-2222-3333-4444-555555555555
//!   empty False 00000000-0000-0000-0000-000000000000
//!   short False 00000000-0000-0000-0000-000000000000
//!   badhex-D False 00000000-0000-0000-0000-000000000000
//!   baddash False 00000000-0000-0000-0000-000000000000
//!   badhex-N False 00000000-0000-0000-0000-000000000000
//!   wide False 00000000-0000-0000-0000-000000000000
//!   wide36 False 00000000-0000-0000-0000-000000000000
//!   False 00000000-0000-0000-0000-000000000000
module Conf;

public class Program
  public static method Show(tag: string, s: string)
    var o: Guid := Guid.Parse("ffffffff-ffff-ffff-ffff-ffffffffffff");
    var ok := Guid.TryParse(s, out o);
    Console.WriteLine(tag + " " + ok.ToString() + " " + o.ToString());
  end Show;
  public static method Main()
    Program.Show("valid-D", "11111111-2222-3333-4444-555555555555");
    Program.Show("valid-N", "11111111222233334444555555555555");
    Program.Show("empty", "");
    Program.Show("short", "1234");
    Program.Show("badhex-D", "11111111-2222-3333-4444-55555555555z");
    Program.Show("baddash", "11111111_2222-3333-4444-555555555555");
    Program.Show("badhex-N", "1111111122223333444455555555555z");
    Program.Show("wide", "日日日日日日日日日日日日日日日日日日日日日日日日日日日日日日日日日日日日");
    Program.Show("wide36", "1111111-日22-3333-4444-555555555555x");
    var ok2 := Guid.TryParse("zz", out var o2);
    Console.WriteLine(ok2.ToString() + " " + o2.ToString());
  end Main;
end Program;
