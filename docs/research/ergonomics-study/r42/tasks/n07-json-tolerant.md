# n07-json-tolerant

*Area:* JSON

Deserialize a JSON service config that has an unknown field and omits an optional one, with a deserializer that ignores unknown fields: `{"name":"api","port":8080,"tls":{"enabled":true,"cert":"/etc/c.pem"},"debug":true}` into `ServiceConfig(Name, Port, Tls: TlsConfig(Enabled, Cert), Retries optional default 3)`. Print `api:8080 tls=True cert=/etc/c.pem retries=3`. Then the same text with the strict default deserializer: print `strict: rejected` from the caught `SerializationException`.
