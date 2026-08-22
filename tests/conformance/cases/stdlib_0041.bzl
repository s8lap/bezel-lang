public class ScopeGuard
  public constructor(scoped action: () -> ());     // runs at disposal unless dismissed
  public method Dismiss();                          // disarms; idempotent
  public readonly property IsDismissed: bool;
  destructor;                                       // invokes action unless dismissed
end ScopeGuard;

public method Register(id: int)
  _registry.Add(id);
  using var g := new ScopeGuard(lambda() => Unregister(id));  // the class's unit-returning
                                                              //   inverse: the action is () -> ()
                                                              //   by construction
  Validate(id);            // may throw
  Publish(id);             // may throw
  g.Dismiss();             // success: keep the registration
end Register;
