method CloneBuffer(): owned Buffer
  var fresh := new Buffer();
  return fresh;
end CloneBuffer;

public static method Create(host: string): owned Connection
