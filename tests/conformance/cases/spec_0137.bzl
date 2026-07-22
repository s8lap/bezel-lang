public class WaferTest
  public SaveResults: bool := false;
  public NotifyUi: bool := false;
  public constructor(zUp: bool) end constructor;
  destructor end destructor;
end WaferTest;

public class Runner
  public static method Main()
    // §9.5 (v0.188): a `with` source must be record-typed. WaferTest is a
    // class, so this is a compile error naming §9.5 — the former §22.11
    // "object initializer block" reading of `with` was retired in v0.188
    // (proposal 0037); see spec_0138 for the remedy this diagnostic names
    // (bind to a local, assign members as statements).
    using var t := new WaferTest(zUp := true) with(SaveResults := true,
      NotifyUi := false
    );
  end Main;
end Runner;
