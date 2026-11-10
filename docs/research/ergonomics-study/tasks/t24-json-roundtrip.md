# t24-json-roundtrip

*Area:* JSON

A record Person(Name: string, Age: int, Tags: Array<string>) — or the closest serializable shape. Serialize Person("Ann", 34, [admin, dev]) to JSON text, print it (`{"Name":"Ann","Age":34,"Tags":["admin","dev"]}` or the format's equivalent), parse it back, and print `roundtrip equal: True` comparing fields.
