public class ScopeGuard
  public constructor(scoped action: () -> ());     // runs at disposal unless dismissed
  public method Dismiss();                          // disarms; idempotent
  public readonly property IsDismissed: bool;
  destructor;                                       // invokes action unless dismissed
end ScopeGuard;
