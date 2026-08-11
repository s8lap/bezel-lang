// ERROR: TakeOwnership may throw (method calls are throwable, above) — on that
//        edge _resource is moved but never reassigned: the WrongOrder shape.
public method TransferToHelper()
  var fresh := new Stream();
  TakeOwnership(move _resource);          // owned argument; helper consumes
  _resource := fresh;                      // restores the normal edge only
end TransferToHelper;
