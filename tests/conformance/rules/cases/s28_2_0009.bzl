//! case: s28_2_0009
//! doc: spec
//! section: 28.2
//! title: the same member under any other name is admitted, optional parameter included
//! rule: The rule is keyed on the **name** `Dispose`, not on the shape of the
//!   declaration: a class member with the identical parameter list under any
//!   other name collides with no compiler-generated member and is admitted,
//!   callable at zero arguments through §22.5's optional parameter.
//! cites: 28.2, 22.5
//! expect: accept
//! issue: 4404
module Conf;

public class Session
  public constructor()
  end constructor;

  public method Close(mode: int := 0)
    Console.WriteLine(mode.ToString());
  end Close;
end Session;

public class Program
  public method Run()
    using var s := new Session();
    s.Close();
  end Run;
end Program;
