#include <map>
#include <string>
#include <vector>
#include <iostream>

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"

using namespace llvm;

enum Token {
    tok_eof = -1,
    tok_def = -2,
    tok_extern = -3,

    tok_identifier = -4,
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

        return tok_identifier;
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
    virtual ~ExprAST() = default;
    virtual Value* codegen() = 0;
};

class NumberExprAST : public ExprAST {
    double Val;

public:
    NumberExprAST(double Val) : Val(Val){}
    Value* codegen() override;
};

class VariableExprAST : public ExprAST {
    std::string Name;

public: 
    VariableExprAST(const std::string &Name) : Name(Name) {}
    Value* codegen() override;
};

class BinaryExprAST : public ExprAST {
    char Op;
    std::unique_ptr<ExprAST> LHS, RHS;

public:
    BinaryExprAST(char op, std::unique_ptr<ExprAST> LHS,
                           std::unique_ptr<ExprAST> RHS) 
                  : Op(op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
    Value* codegen() override;
};

class CallExprAST : public ExprAST {
    std::string Callee;
    std::vector<std::unique_ptr<ExprAST>> Args;
public:
    CallExprAST(const std::string &Callee, 
                std::vector<std::unique_ptr<ExprAST>> Args)
                : Callee(Callee), Args(std::move(Args)) {}
    Value* codegen() override;
};

class PrototypeAST {
    std::string Name;
    std::vector<std::string> Args;
public:
    PrototypeAST(const std::string &Name, std::vector<std::string> Args) 
                : Name(Name), Args(std::move(Args)) {}
    
    const std::string &getName() const {return Name;}

    Function* codegen();
};

class FunctionAST {
    std::unique_ptr<PrototypeAST> Proto;
    std::unique_ptr<ExprAST> Body;

public:
    FunctionAST(std::unique_ptr<PrototypeAST> Proto,
               std::unique_ptr<ExprAST> Body)
               : Proto(std::move(Proto)), Body(std::move(Body)) {}
    Function* codegen();
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
    auto Result = std::make_unique<NumberExprAST>(NumVal);
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
        case tok_identifier:
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
    if (CurTok != tok_identifier) return LogErrorP("Expected function name in prototype");
    std::string FnName = IdentifierStr;
    getNextToken();
    if (CurTok != '(') return LogErrorP("Expected '(' in prototype");

    std::vector<std::string> ArgNames;
    while (getNextToken() == tok_identifier) {
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

/****** Code Generation ******/
static std::unique_ptr<LLVMContext> TheContext;
static std::unique_ptr<IRBuilder<>> Builder;
static std::unique_ptr<Module>      TheModule;
static std::map<std::string, Value*> NameValues;

llvm::Value *LogErrorV(const char* Str) {
    LogError(Str);
    return nullptr;
}

Value* NumberExprAST::codegen() {
    return ConstantFP::get(*TheContext, APFloat(Val));
}

Value* VariableExprAST::codegen() {
    Value* V = NameValues[Name];
    if (!V) LogErrorV("Unknown variable name");
    return V;
}

Value* BinaryExprAST::codegen() {
    Value *L = LHS->codegen();
    Value *R = RHS->codegen();
    if (!L || !R) return nullptr;

    switch (Op) {
        case '+':
            return Builder->CreateFAdd(L, R, "addtmp");
        case '-':
            return Builder->CreateFAdd(L, R, "subtmp");
        case '*':
            return Builder->CreateFAdd(L, R, "multmp");
        case '<':
            L = Builder->CreateFCmpULT(L, R, "cmptmp");
            // conver bool 0/1 to double 0.0/1.0
            return Builder->CreateUIToFP(L, Type::getDoubleTy(*TheContext), "booltmp");
        default:
            return LogErrorV("invalid binary operator");
    }
}

Value* CallExprAST::codegen() {
    Function* CalleeF = TheModule->getFunction(Callee);
    if (!CalleeF) return LogErrorV("Unknown function referenced");
    if (CalleeF->arg_size() != Args.size()) 
        return LogErrorV("Incorrect # arguments passed");
    
    std::vector<Value*> ArgsV;
    for (unsigned i = 0, e = Args.size(); i != e; i++) {
        ArgsV.push_back(Args[i]->codegen());
        if (!ArgsV.back()) return nullptr;
    }

    return Builder->CreateCall(CalleeF, ArgsV, "calltmp");

}

Function* PrototypeAST::codegen() {
    std::vector<Type*> Doubles(Args.size(), Type::getDoubleTy(*TheContext));

    FunctionType* FT =
        FunctionType::get(Type::getDoubleTy(*TheContext), Doubles, false);
    Function* F = 
        Function::Create(FT, Function::ExternalLinkage, Name, TheModule.get());

    // set names for all arguements
    unsigned Idx = 0;
    for (auto &Arg : F->args())
        Arg.setName(Args[Idx++]);
    return F;
}

Function* FunctionAST::codegen() {
    // check if the func has been created with 'extern'
    Function* TheFunction = TheModule->getFunction(Proto->getName());
    if (!TheFunction) TheFunction = Proto->codegen();
    if (!TheFunction) return nullptr;

    // create a new BB to start insertion into
    BasicBlock* BB = BasicBlock::Create(*TheContext, "entry", TheFunction);
    Builder->SetInsertPoint(BB);

    // record the function arguments in the NameValues map
    NameValues.clear();
    for (auto &Arg : TheFunction->args()) {
        NameValues[std::string(Arg.getName())] = &Arg;
    }

    if (Value* RetVal = Body->codegen()) {
        Builder->CreateRet(RetVal);
        verifyFunction(*TheFunction);
        return TheFunction;
    }

    // read body wrong, remove fuction
    TheFunction->eraseFromParent();
    return nullptr;
}



/****** top-level parsing and JIT driver ******/
static void InitializeModule() {
    TheContext = std::make_unique<LLVMContext>();
    TheModule  = std::make_unique<Module>("my toy jit", *TheContext);
    Builder    = std::make_unique<IRBuilder<>>(*TheContext);
}

static void HandleDefinition() {
    if (auto FnAST = ParseDefinition()) {
        if (auto *FnIR = FnAST->codegen()) {
            fprintf(stderr, "Parsed a function definition: ");
            FnIR->print(errs());
            fprintf(stderr, "\n");
        }
    } else getNextToken();  // Skip token for error recovery.
}

static void HandleExtern() {
    if (auto ProtoAST = ParseExtern()) {
        if (auto *FnIR = ProtoAST->codegen()) {
            fprintf(stderr, "Parsed an extern: ");
            FnIR->print(errs());
            fprintf(stderr, "\n");
        }
    } else getNextToken();
}

static void HandleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function.
  if (auto FnAST = ParseTopLevelExpr()) {
        if (auto *FnIR = FnAST->codegen()) {
            fprintf(stderr, "Parsed a top-level expr: ");
            FnIR->print(errs());
            fprintf(stderr, "\n");
            // remove the anonymous expression
            FnIR->eraseFromParent();
        }
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

  InitializeModule();

  MainLoop();

  TheModule->print(errs(), nullptr);

  return 0;
}
