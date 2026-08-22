Literal         ::= IntLiteral | FloatLiteral | CharLiteral
                  | StringLiteral | InterpString
                  | "true" | "false" | "null"
ConstExpr       ::= Expression           /* §17.14 constant expression */
Constant        ::= "-"? (IntLiteral | FloatLiteral)
                  | CharLiteral
                  | StringLiteral | "true" | "false" | "null"
                  | QualifiedName        /* enum constant, choice case (§35.5), or const reference */
