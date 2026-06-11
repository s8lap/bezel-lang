public method TransferToHelper()
  var fresh := new Stream();
  TakeOwnership(move _resource);          // owned argument; helper consumes
  _resource := fresh;
end TransferToHelper;
