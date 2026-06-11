public method TransferToHelperSafe()
  var fresh := new Stream();
  var taken := move _resource;
  _resource := fresh;                      // restore before the call that may throw
  TakeOwnership(taken);                    // owned argument, no field state at risk
end TransferToHelperSafe;
