Identifier      ::= Letter IdentCont* | "_" IdentCont+
                              /* The bare single-character `_` is NOT an
                                 Identifier: it is lexed as its own reserved
                                 token — the discard (§1.3, §17.13, B.1) — and
                                 no Identifier production ever matches it.
                                 Identifiers may begin with or contain `_`
                                 (`_camelCase`, `_f`, `snake_case`); only the
                                 bare token is reserved. */
IdentStart      ::= Letter | "_"
IdentCont       ::= IdentStart | Digit
Letter          ::= /any Unicode letter/
Digit           ::= "0".."9"
HexDigit        ::= Digit | "a".."f" | "A".."F"

IntLiteral      ::= DecLiteral | HexLiteral | BinLiteral
DecLiteral      ::= Digit ("_"* Digit)* IntSuffix?
HexLiteral      ::= "0x" HexDigit ("_"* HexDigit)* IntSuffix?
BinLiteral      ::= "0b" ("0" | "1") ("_"* ("0" | "1"))* IntSuffix?
IntSuffix       ::= "L" | "U" | "UL" | "LU"   /* case-insensitive */

FloatLiteral    ::= DigitGroup "." DigitGroup Exponent? FloatSuffix?
                  | DigitGroup Exponent FloatSuffix?
                  | DigitGroup FloatSuffix
DigitGroup      ::= Digit ("_"* Digit)*
Exponent        ::= ("e" | "E") ("+" | "-")? DigitGroup
FloatSuffix     ::= "f" | "F" | "m" | "M"     /* m -> decimal */

CharLiteral     ::= "'" CharContent "'"
StringLiteral   ::= '"' StringContent* '"'
                  | '@"' VerbatimContent* '"'
InterpString    ::= '$"' (InterpPart | StringContent)* '"'
                  | '$@"' (InterpPart | VerbatimContent)* '"'
InterpPart      ::= "{" Expression ("," "-"? IntLiteral)? (":" FormatSpec)? "}"
