//! case: s28_8_0004
//! doc: spec
//! section: 28.8
//! title: the take-restore-return idiom hands an owned field's contents to the caller
//! rule: Moving an owned field's contents out uses the `move` expression form:
//!   the field is left moved-from and must be reassigned on every path to the
//!   method's exit, so the pattern for "atomically take this field's contents
//!   and return them as owned" builds the replacement first, takes, restores,
//!   then returns the taken value (§28.8, §28.8.1).
//! cites: 28.8, 28.8.1, 28.5
//! expect: run
//! output: first
//!   dispose first
//!   after
//!   dispose second
module Conf;

public class Widget
  public property Tag: string;

  public constructor(tag: string)
    Tag := tag;
  end constructor;

  destructor
    Console.WriteLine($"dispose {Tag}");
  end destructor;
end Widget;

public class Holder
  private owned _current: Widget;

  public constructor()
    _current := new Widget("first");
  end constructor;

  public method ReplaceAndReturn(): owned Widget
    var fresh := new Widget("second");
    var taken := move _current;
    _current := fresh;
    return taken;
  end ReplaceAndReturn;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    do
      using var prev := h.ReplaceAndReturn();
      Console.WriteLine(prev.Tag);
    end;
    Console.WriteLine("after");
  end Main;
end Program;
