#include <map>
#include <string>
#include <vector>
#include <iostream>

#include "include/KaleidoscopeJIT.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"

using namespace llvm;
using namespace llvm::orc;

enum Token {
    tok_eof = -1,
    tok_def = -2,
    tok_extern = -3,

    tok_identifier = -4,
    tok_number = -5,

    // control
    tok_if = -6,
    tok_then = -7,
    tok_else = -8,    
    tok_for = -9,
    tok_in = -10,
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
        } else if (IdentifierStr == "if") {
            return tok_if;
        } else if (IdentifierStr == "then") {
            return tok_then;
        } else if (IdentifierStr == "else") {
            return tok_else;
        }else if (IdentifierStr == "for") {
            return tok_for;
        } else if (IdentifierStr == "in") {
            return tok_in;
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

class IfExprAST : public ExprAST {
    std::unique_ptr<ExprAST>Cond, Then, Else;

public:
    IfExprAST(std::unique_ptr<ExprAST> Cond, std::unique_ptr<ExprAST> Then,
              std::unique_ptr<ExprAST> Else)
        : Cond(std::move(Cond)), Then(std::move(Then)), Else(std::move(Else)) {}
    Value* codegen() override;
};

class ForExprAST : public ExprAST {
  std::string VarName;
  std::unique_ptr<ExprAST> Start, End, Step, Body;

public:
  ForExprAST(const std::string &VarName, std::unique_ptr<ExprAST> Start,
             std::unique_ptr<ExprAST> End, std::unique_ptr<ExprAST> Step,
             std::unique_ptr<ExprAST> Body)
    : VarName(VarName), Start(std::move(Start)), End(std::move(End)),
      Step(std::move(Step)), Body(std::move(Body)) {}

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

static std::unique_ptr<ExprAST> ParseIfExpr() {
    getNextToken(); //eat "if"

    // condition
    auto Cond = ParseExpression();
    if (!Cond) return nullptr;
    if (CurTok != tok_then) return LogError("expected then");

    getNextToken(); // eat "then"

    auto Then = ParseExpression();
    if (!Then) return nullptr;
    if (CurTok != tok_else) return LogError("expected else");
    getNextToken(); // eat "else"

    auto Else = ParseExpression();
    if (!Else) return nullptr;
    
    return std::make_unique<IfExprAST>(std::move(Cond), std::move(Then), std::move(Else));
}

// forexpr ::= 'for' identifier '=' expr ',' expr (',' expr)? 'in' expression
static std::unique_ptr<ExprAST> ParseForExpr() {
    getNextToken();  // eat the for

    if (CurTok != tok_identifier) return LogError("expected identifier after for");

    std::string IdName = IdentifierStr;
    getNextToken();  // eat identifier

    if (CurTok != '=') return LogError("expected '=' after for");
    getNextToken();  // eat '='

    auto Start = ParseExpression();
    if (!Start) return nullptr;
    if (CurTok != ',') return LogError("expected ',' after for start value");
    getNextToken();

    auto End = ParseExpression();
    if (!End) return nullptr;

    // the step value is optional.
    std::unique_ptr<ExprAST> Step;
    if (CurTok == ',') {
        getNextToken();
        Step = ParseExpression();
        if (!Step) return nullptr;
    }

    if (CurTok != tok_in) return LogError("expected 'in' after for");
    getNextToken();  // eat 'in'

    auto Body = ParseExpression();
    if (!Body) return nullptr;

    return std::make_unique<ForExprAST>(IdName, std::move(Start), std::move(End), 
                                                std::move(Step),  std::move(Body));
    }


static std::unique_ptr<ExprAST> ParsePrimary() {
    switch (CurTok) {
        case tok_identifier:
            return ParseIdentifierExpr();
        case tok_number:
            return ParseNumberExpr();
        case '(':
            return ParseParenExpr();
        case tok_if:
            return ParseIfExpr();
        case tok_for:
            return ParseForExpr();
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

static std::unique_ptr<legacy::FunctionPassManager> TheFPM;
static std::unique_ptr<KaleidoscopeJIT> TheJIT;
// hold the most recent prototype for each function
static std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos; 
static ExitOnError ExitOnErr;

llvm::Value *LogErrorV(const char* Str) {
    LogError(Str);
    return nullptr;
}

Function* getFunction(std::string Name) {
    if (auto* F = TheModule->getFunction(Name)) return F;

    auto FI = FunctionProtos.find(Name);
    if (FI != FunctionProtos.end()) return FI->second->codegen();

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
            return Builder->CreateFSub(L, R, "subtmp");
        case '*':
            return Builder->CreateFMul(L, R, "multmp");
        case '<':
            L = Builder->CreateFCmpULT(L, R, "cmptmp");
            // conver bool 0/1 to double 0.0/1.0
            return Builder->CreateUIToFP(L, Type::getDoubleTy(*TheContext), "booltmp");
        default:
            return LogErrorV("invalid binary operator");
    }
}

Value* CallExprAST::codegen() {
    Function* CalleeF = getFunction(Callee);
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

Value *IfExprAST::codegen() {
    Value *CondV = Cond->codegen();
    if (!CondV) return nullptr;

    // convert condition to a bool by comparing non-equal to 0.0.
    CondV = Builder->CreateFCmpONE(
        CondV, ConstantFP::get(*TheContext, APFloat(0.0)), "ifcond");

    Function* TheFunction = Builder->GetInsertBlock()->getParent();

    // create blocks for the then and else cases. 
    // Insert the 'then' block at the end of the function.
    BasicBlock* ThenBB  = BasicBlock::Create(*TheContext, "then", TheFunction);
    BasicBlock* ElseBB  = BasicBlock::Create(*TheContext, "else");
    BasicBlock* MergeBB = BasicBlock::Create(*TheContext, "ifcont");

    Builder->CreateCondBr(CondV, ThenBB, ElseBB);

    // emit then value.
    Builder->SetInsertPoint(ThenBB);

    Value* ThenV = Then->codegen();
    if (!ThenV) return nullptr;

    Builder->CreateBr(MergeBB);
    // codegen of 'Then' can change the current block, update ThenBB for the PHI.
    ThenBB = Builder->GetInsertBlock();

    // emit else block.
    TheFunction->getBasicBlockList().push_back(ElseBB);
    Builder->SetInsertPoint(ElseBB);

    Value* ElseV = Else->codegen();
    if (!ElseV) return nullptr;

    Builder->CreateBr(MergeBB);
    // codegen of 'Else' can change the current block, update ElseBB for the PHI.
    ElseBB = Builder->GetInsertBlock();

    // emit merge block.
    TheFunction->getBasicBlockList().push_back(MergeBB);
    Builder->SetInsertPoint(MergeBB);
    PHINode* PN = Builder->CreatePHI(Type::getDoubleTy(*TheContext), 2, "iftmp");

    PN->addIncoming(ThenV, ThenBB);
    PN->addIncoming(ElseV, ElseBB);
    return PN;
}

/* Output for-loop as:
 *   ...
 *  start = startexpr
 *   goto loop
 * loop:
 *   variable = phi [start, loopheader], [nextvariable, loopend]
 *   ...
 *   bodyexpr
 *   ...
 * loopend:
 *   step = stepexpr
 *   nextvariable = variable + step
 *   endcond = endexpr
 *   br endcond, loop, endloop
 * outloop: 
 */
Value* ForExprAST::codegen() {
    // emit the start code first, without 'variable' in scope.
    Value *StartVal = Start->codegen();
    if (!StartVal) return nullptr;

    // make the new basic block for the loop header, inserting after current block
    Function *TheFunction = Builder->GetInsertBlock()->getParent();
    BasicBlock *PreheaderBB = Builder->GetInsertBlock();
    BasicBlock *LoopBB = BasicBlock::Create(*TheContext, "loop", TheFunction);

    // insert an explicit fall through from the current block to the LoopBB.
    Builder->CreateBr(LoopBB);

    // start insertion in LoopBB.
    Builder->SetInsertPoint(LoopBB);

    // start the PHI node with an entry for Start.
    PHINode *Variable = Builder->CreatePHI(Type::getDoubleTy(*TheContext), 2, VarName);
    Variable->addIncoming(StartVal, PreheaderBB);

    // within the loop, the variable is defined equal to the PHI node 
    // if it shadows an existing variable, we have to restore it, so save it now.
    Value *OldVal = NameValues[VarName];
    NameValues[VarName] = Variable;

    // emit the body of the loop
    if (!Body->codegen()) return nullptr;

    // emit the step value.
    Value *StepVal = nullptr;
    if (Step) {
        StepVal = Step->codegen();
        if (!StepVal) return nullptr;
    } else {
        // if not specified, use 1.0.
        StepVal = ConstantFP::get(*TheContext, APFloat(1.0));
    }

    Value* NextVar = Builder->CreateFAdd(Variable, StepVal, "nextvar");

    // compute the end condition.
    Value* EndCond = End->codegen();
    if (!EndCond) return nullptr;

    // convert condition to a bool by comparing non-equal to 0.0.
    EndCond = Builder->CreateFCmpONE(EndCond, ConstantFP::get(*TheContext, APFloat(0.0)), "loopcond");

    // create the "after loop" block and insert it.
    BasicBlock *LoopEndBB = Builder->GetInsertBlock();
    BasicBlock *AfterBB = BasicBlock::Create(*TheContext, "afterloop", TheFunction);

    // insert the conditional branch into the end of LoopEndBB.
    Builder->CreateCondBr(EndCond, LoopBB, AfterBB);

    // new code will be inserted in AfterBB.
    Builder->SetInsertPoint(AfterBB);

    // add a new entry to the PHI node for the backedge.
    Variable->addIncoming(NextVar, LoopEndBB);

    // restore the unshadowed variable.
    if (OldVal) NameValues[VarName] = OldVal;
    else NameValues.erase(VarName);

    // for expr always returns 0.0.
    return Constant::getNullValue(Type::getDoubleTy(*TheContext));
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
    auto& P = *Proto;
    FunctionProtos[Proto->getName()] = std::move(Proto);
    Function* TheFunction = getFunction(P.getName());
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
        TheFPM->run(*TheFunction); // optimize the function
        return TheFunction;
    }

    // read body wrong, remove fuction
    TheFunction->eraseFromParent();
    return nullptr;
}



/****** top-level parsing and JIT driver ******/
// static void InitializeModule() {
//     TheContext = std::make_unique<LLVMContext>();
//     TheModule  = std::make_unique<Module>("my toy jit", *TheContext);
//     Builder    = std::make_unique<IRBuilder<>>(*TheContext);
// }


static void InitializeModuleAndPassManager(void) {
    // open a new context and module
    TheContext = std::make_unique<LLVMContext>();
    
    TheModule  = std::make_unique<Module>("my toy jit", *TheContext);
    TheModule->setDataLayout(TheJIT->getDataLayout());
    
    Builder    = std::make_unique<IRBuilder<>>(*TheContext);

    // create a new pass manager attached to it
    TheFPM = std::make_unique<legacy::FunctionPassManager>(TheModule.get());
    // peephole optimization
    TheFPM->add(createInstructionCombiningPass());
    // reassociate expressions
    TheFPM->add(createReassociatePass());
    // eliminate common subexpressions.
    TheFPM->add(createGVNPass());
    // simplify the control flow graph (deleting unreachable blocks, etc).
    TheFPM->add(createCFGSimplificationPass());

    TheFPM->doInitialization();
}

static void HandleDefinition() {
    if (auto FnAST = ParseDefinition()) {
        if (auto *FnIR = FnAST->codegen()) {
            fprintf(stderr, "Parsed a function definition: ");
            FnIR->print(errs());
            fprintf(stderr, "\n");
            ExitOnErr(TheJIT->addModule(ThreadSafeModule(std::move(TheModule), std::move(TheContext))));
            InitializeModuleAndPassManager();
        }
    } else getNextToken();  // Skip token for error recovery.
}

static void HandleExtern() {
    if (auto ProtoAST = ParseExtern()) {
        if (auto *FnIR = ProtoAST->codegen()) {
            fprintf(stderr, "Parsed an extern: ");
            FnIR->print(errs());
            fprintf(stderr, "\n");
            FunctionProtos[ProtoAST->getName()] = std::move(ProtoAST);
        }
    } else getNextToken();
}

static void HandleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function.
  if (auto FnAST = ParseTopLevelExpr()) {
        if (auto* FnIR = FnAST->codegen()) {
            
            fprintf(stderr, "Parsed a top-level expr: ");
            FnIR->print(errs());
            // create a ResourceTracker to track JIT'd memory a
            auto RT  = TheJIT->getMainJITDylib().createResourceTracker();
            auto TSM = ThreadSafeModule(std::move(TheModule), std::move(TheContext));
            ExitOnErr(TheJIT->addModule(std::move(TSM), RT));
          
            InitializeModuleAndPassManager();

            // search the JIT for the __anon_expr symbol.
            auto ExprSymbol = ExitOnErr(TheJIT->lookup("__anon_expr"));
            assert(ExprSymbol && "Function not found");

            // get the symbol's address and cast it to the right type
            double(*FP)() = (double(*)())(intptr_t)ExprSymbol.getAddress();
           
            fprintf(stderr, "Evaluated to %f\n", FP());

            // delete the anonymous expression module from the JIT
            ExitOnErr(RT->remove());
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

/****** "Library" functions that can be "extern'd" from user code ******/
#ifdef _WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

extern "C" DLLEXPORT double putchard(double X) {
  fputc((char)X, stderr);
  return 0;
}

extern "C" DLLEXPORT double printd(double X) {
  fprintf(stderr, "%f\n", X);
  return 0;
}


int main() {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    InstallBinop();

    fprintf(stderr, "ready> ");
    getNextToken();

    //   InitializeModule();
  
    TheJIT = ExitOnErr(KaleidoscopeJIT::Create());
    InitializeModuleAndPassManager();
    MainLoop();

    TheModule->print(errs(), nullptr);

    return 0;
}
