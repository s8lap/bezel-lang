module Svc.Config;

import System.Serialization;

public record TlsConfig(Enabled: bool serialized "enabled", Cert: string serialized "cert"): ISerializable, IDeserializable;

public record ServiceConfig(
  Name: string serialized "name",
  Port: int serialized "port",
  Tls: TlsConfig serialized "tls",
  Retries: int serialized "retries" optional
): ISerializable, IDeserializable;

public class Program
  public static method Main()
    const text := """{"name":"api","port":8080,"tls":{"enabled":true,"cert":"/etc/c.pem"},"debug":true}""";
    var lenient := new JsonOptions(false, 2, false, true, 64);
    using var d := new JsonDeserializer(text, lenient);
    var cfg := ServiceConfig.Deserialize(d);
    var retries := (if cfg.Retries = 0 then 3 else cfg.Retries);
    Console.WriteLine($"{cfg.Name}:{cfg.Port} tls={cfg.Tls.Enabled} cert={cfg.Tls.Cert} retries={retries}");
    try
      using var strict := new JsonDeserializer(text);
      _ := ServiceConfig.Deserialize(strict);
      Console.WriteLine("strict: accepted");
    catch
      SerializationException => Console.WriteLine("strict: rejected")
    end;
  end Main;
end Program;
