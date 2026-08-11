public method TransferToHelperSafe()
  var fresh := new Stream();
  var taken := move _resource;
  _resource := fresh;                      // restore before the call that may throw
  TakeOwnership(move taken);               // owned argument (call-site marker, §22.3), no field state at risk
end TransferToHelperSafe;
