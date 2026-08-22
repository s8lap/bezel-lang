Register(id);
using var g := new ScopeGuard(lambda() => Unregister(id));
DoWork();
g.Dismiss();             // success: keep the registration
