#ifndef __LEXER_
#define __LEXER_
#include <string>
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

int main() {
    while(1) {
        int tok = gettoken();
        std::cout << "Token: " << tok << "\n";
    }
}

#endif