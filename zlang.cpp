#include <map>
#include <string>
#include <vector>
#include <iostream>

enum Token {
    tok_eof = -1,
    tok_def = -2,
    tok_extern = -3,

    tok_indentifier = -4,
    tok_number = -5,
};

static std::string IdentifierStr;
static double NumVal;

static std::map<char, int> BinopPrecedence;

static int gettoken() {
    static int LastChar = ' ';

    // skip whitespaces
    while (isspace(LastChar)) {
        LastChar = getchar();
    }

    if (isalpha(LastChar)) {
        IdentifierStr = LastChar;
        while (isalnum(LastChar = getchar())) {
            IdentifierStr += LastChar;
        }

        if (IdentifierStr == "def") {
            return tok_def;
        } else if (IdentifierStr == "extern") {
            return tok_extern;
        }

        return tok_indentifier;
    }

    if (isdigit(LastChar)|| LastChar == '.') {
        std::string NumStr;
        do {
            NumStr += LastChar;
            LastChar = getchar();
        } while(isdigit(LastChar) || LastChar == '.');

        NumVal = strtod(NumStr.c_str(), 0);
        return tok_number;
    }

    // comments
    if (LastChar == '#') {
        do {
            LastChar = getchar();
        } while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');
        
        if (LastChar != EOF) {
            // recursively call myself
            return gettoken();
        }
    }

    if (LastChar == EOF) {
        return tok_eof;
    }

    // such as '+', just return the ascii value.
    int ThisChar = LastChar;
    LastChar = getchar();
    return ThisChar;

}

// Base class for all expression nodes.
class ExprAST {
public:
    virtual ~ExprAST() {}
};

class NumberExperAST : public ExprAST {
    double Val;

public:
    NumberExperAST(double Val) : Val(Val){}
};

class VariableExprAST : public ExprAST {
    std::string Name;

public: 
    VariableExprAST(const std::string &Name) : Name(Name) {}
};

class BinaryExprAST : public ExprAST {
    char Op;
    std::unique_ptr<ExprAST> LHS, RHS;

public:
    BinaryExprAST(char op, std::unique_ptr<ExprAST> LHS,
                           std::unique_ptr<ExprAST> RHS) 
                  : Op(op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
};

class CallExprAST : public ExprAST {
    std::string Callee;
    std::vector<std::unique_ptr<ExprAST>> Args;
public:
    CallExprAST(const std::string &Callee, 
                std::vector<std::unique_ptr<ExprAST>> Args)
                : Callee(Callee), Args(std::move(Args)) {}
};

class PrototypeAST {
    std::string Name;
    std::vector<std::string> Args;
public:
    PrototypeAST(const std::string &Name, std::vector<std::string> Args) 
                : Name(Name), Args(std::move(Args)) {}
    
    const std::string &getName() const {return Name;}
};

class FunctionAST {
    std::unique_ptr<PrototypeAST> Proto;
    std::unique_ptr<ExprAST> Body;

public:
    FunctionAST(std::unique_ptr<PrototypeAST> Proto,
               std::unique_ptr<ExprAST> Body)
               : Proto(std::move(Proto)), Body(std::move(Body)) {}
};

static int CurTok;
static int getNextToken() {
    return CurTok = gettoken();
}

static std::unique_ptr<ExprAST> ParseExpression();
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec, std::unique_ptr<ExprAST> LHS);
std::unique_ptr<ExprAST> LogError(const char* Str) {
    fprintf(stderr, "Error: %s\n", Str);
    return nullptr;
}

std::unique_ptr<PrototypeAST> LogErrorP(const char* Str) {
    LogError(Str);
    return nullptr;
}

// Parser

// numberexpr ::= number
static std::unique_ptr<ExprAST> ParseNumberExpr() {
    auto Result = std::make_unique<NumberExperAST>(NumVal);
    getNextToken();
    return std::move(Result);
}

// parenexpr ::= '(' expression ')'
static std::unique_ptr<ExprAST> ParseParenExpr() {
    getNextToken(); // eat'('
    auto V = ParseExpression();
    if (!V) return nullptr;
    if (CurTok == ')') {
        getNextToken(); // eat')'
        return V;
    } else {
        return LogError("expected ')'");
    }
}

/* identifierexpr
 *   ::= identifier
 *   ::= identifier '(' expression* ')' */
static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
    std::string IdName = IdentifierStr;

    getNextToken(); // eat identifier
    
    if (CurTok == '(') { // function calls
        getNextToken();  // eat '('
        std::vector<std::unique_ptr<ExprAST>> Args;
        if (CurTok != ')') {
            while (true) {
                if (auto Arg =ParseExpression()) {
                    Args.push_back(std::move(Arg));
                } else return nullptr;

                if (CurTok == ')') {
                    getNextToken(); // eat')'
                    return std::make_unique<CallExprAST>(IdName, std::move(Args));
                }
                else if (CurTok == ',') {
                    getNextToken(); // eat ','
                    continue;
                } else {
                    return LogError("Expected ')' or ',' in argument list");
                } 
            }
        } else {
            getNextToken();
            return std::make_unique<CallExprAST>(IdName, std::move(Args));
        }
    } else { // simple variable ref
        return std::make_unique<VariableExprAST>(IdName);
    }
}

static std::unique_ptr<ExprAST> ParsePrimary() {
    switch (CurTok) {
        case tok_indentifier:
            return ParseIdentifierExpr();
        case tok_number:
            return ParseNumberExpr();
        case '(':
            return ParseParenExpr();
        default: 
            return LogError("unknown token when expecting an expression");
    }
}

static int GetTokPrecedence() {
    if (!isascii(CurTok)) return -1;
    int TokPrec = BinopPrecedence[CurTok];
    if (TokPrec <= 0) return -1;
    else return TokPrec;
}

static void InstallBinop() {
    BinopPrecedence['<'] = 10;
    BinopPrecedence['>'] = 10;
    BinopPrecedence['+'] = 20;
    BinopPrecedence['-'] = 20;
    BinopPrecedence['*'] = 40;
    BinopPrecedence['/'] = 40;
}

// expression ::= primary binoprhs([binop,primaryexpr])
static std::unique_ptr<ExprAST> ParseExpression() {
    auto LHS = ParsePrimary();
    if (!LHS) return nullptr;
    return ParseBinOpRHS(0, std::move(LHS));
}

// binoprhs ::= ('+' primary)*
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec, std::unique_ptr<ExprAST> LHS) {
    while (true) {
        int TokPrec = GetTokPrecedence();
        if (TokPrec < ExprPrec) return LHS;
        else {
            int BinOp = CurTok; 
            getNextToken();  // eat binop
            auto RHS = ParsePrimary();
            if (!RHS) return nullptr;
            else {
                int NextPrec = GetTokPrecedence();
                if (TokPrec < NextPrec) {
                    RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
                    if (!RHS) return nullptr;
                }
                LHS = std::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS));
            }
        }
    }
}


// prototype ::= id '(' id* ')'
static std::unique_ptr<PrototypeAST> ParsePrototype() {
    if (CurTok != tok_indentifier) return LogErrorP("Expected function name in prototype");
    std::string FnName = IdentifierStr;
    getNextToken();
    if (CurTok != '(') return LogErrorP("Expected '(' in prototype");

    std::vector<std::string> ArgNames;
    while (getNextToken() == tok_indentifier) {
        ArgNames.push_back(IdentifierStr);
    }
    if (CurTok != ')') return LogErrorP("Expected ')' in prototype");
    getNextToken(); // eat ')'
    return std::make_unique<PrototypeAST>(FnName, ArgNames);
}

// definition ::= 'def' prototype expression
static std::unique_ptr<FunctionAST> ParseDefinition() {
    getNextToken(); // eat def
    auto Proto = ParsePrototype();
    if (!Proto) return nullptr;
    if (auto E = ParseExpression()) {
        return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
    } else return nullptr;
}

// external ::= 'extern' prototype
static std::unique_ptr<PrototypeAST> ParseExtern() {
  getNextToken();  // eat extern.
  return ParsePrototype();
}

// toplevelexpr ::= expression
static std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
    if (auto E = ParseExpression()) {
        auto Proto = std::make_unique<PrototypeAST>("__anon_expr", std::vector<std::string>());
        return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
    } else return nullptr;
}


static void HandleDefinition() {
    if (ParseDefinition()) {
        fprintf(stderr, "Parsed a function definition.\n");
    } else getNextToken();  // Skip token for error recovery.
}

static void HandleExtern() {
  if (ParseExtern()) {
    fprintf(stderr, "Parsed an extern\n");
  } else getNextToken();
}

static void HandleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function.
  if (ParseTopLevelExpr()) {
    fprintf(stderr, "Parsed a top-level expr\n");
  } else getNextToken();
}

// top ::= definition | external | expression | ';'
static void MainLoop() {
    while (true) {
        fprintf(stderr, "ready> ");
        switch (CurTok) {
            case tok_eof: return;
            case ';':
                getNextToken();
                break;
            case tok_def:
                HandleDefinition();
                break;
            case tok_extern:
                HandleExtern();
                break;
            default: 
                HandleTopLevelExpression();
                break;
        }
    }
}


int main() {
  InstallBinop();

  fprintf(stderr, "ready> ");
  getNextToken();

  MainLoop();

  return 0;
}