//! case: s2_3_9901
//! doc: spec
//! section: 2.3
//! title: a user class named StringBuilder keeps its own writable Length
//! rule: every other always-available name may be shadowed by local declarations under the normal
//!   name-resolution rules
//! cites: 2.3, 2.7, 23.1
//! expect: run
//! stdout: 8
//! issue: 9250
//! notes: Harvested from the #9250 review (acc_user_string_shadow), the admitting twin of l3_1_9901.
//!   `StringBuilder` is an auto-imported name (2.3), so the user's declaration shadows it and its
//!   public field is an ordinary writable location: 0, then 7, then 8.
module Conf;

public class StringBuilder
  public Length: int := 0;
end StringBuilder;

public class Program
  public static method Main()
    var sb := new StringBuilder();
    sb.Length := 7;
    sb.Length++;
    Console.WriteLine(sb.Length);
  end Main;
end Program;
