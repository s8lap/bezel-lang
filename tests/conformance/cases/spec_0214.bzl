public method Right2(replace: bool): owned Stream?
  if not replace then return null; end;
  var fresh := new Stream();
  var old := move _resource;
  _resource := fresh;
  return old;
end Right2;
