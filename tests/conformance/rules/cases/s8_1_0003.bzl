//! case: s8_1_0003
//! doc: spec
//! section: 8.1
//! title: GetType is callable on a task receiver
//! rule: public method GetType(): Type;
//! cites: 8.1, 8.2, 30.3
//! expect: accept
//! issue: 4630
module Conf;

public class Program
  public static method Probe(t: Task<int>): string
    return t.GetType().Name;
  end Probe;
end Program;
