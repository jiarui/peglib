#include "peglib.h"
#include <string>
#include <iostream>
using namespace peg;

static void testAndExpr() {
    const auto grammar = &terminal('a');

    {
        
        std::string input = "a";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().begin());
    }

    {
        std::string input = "b";
        Context context(input);
        bool ok = grammar(context);
        assert(!ok);
        assert(context.mark() == context.get_input().begin());
    }
}

static void testAlternationExpr() {
    const auto grammar = terminal('a') | 'b' | 'c';

    {
        const std::string input = "a";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().end());
    }

    {
        const std::string input = "b";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().end());
    }

    {
        const std::string input = "c";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().end());
    }

    {
        const std::string input = "d";
        Context context(input);
        bool ok = grammar(context);
        assert(!ok);
        assert(context.mark() == context.get_input().begin());
    }
}

static void testZeroOrMoreExpr() {
    const auto grammar = *terminal('a');

    {
        const std::string input = "a";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().end());
    }

    {
        const std::string input = "aa";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().end());
    }

    {
        const std::string input = "aaa";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().end());
    }

    {
        const std::string input = "b";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().begin());
    }

    {
        const std::string input = "bb";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().begin());
    }

    {
        const std::string input = "bbb";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().begin());
    }

    {
        const std::string input = "";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().begin());
    }
}


int main(int argc, char* argv[]){
    testAndExpr();
    testAlternationExpr();
    testZeroOrMoreExpr();
    return 0;
}
