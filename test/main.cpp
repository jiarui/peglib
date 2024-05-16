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

static void testOneOrMoreExpr() {
    const auto grammar = +terminal('a');

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
        assert(!ok);
        assert(context.mark() == context.get_input().begin());
    }

    {
        const std::string input = "bb";
        Context context(input);
        bool ok = grammar(context);
        assert(!ok);
        assert(context.mark() == context.get_input().begin());
    }

    {
        const std::string input = "bbb";
        Context context(input);
        bool ok = grammar(context);
        assert(!ok);
        assert(context.mark() == context.get_input().begin());
    }

    {
        const std::string input = "";
        Context context(input);
        bool ok = grammar(context);
        assert(!ok);
        assert(context.mark() == context.get_input().begin());
    }
}

static void testNTimesExpr() {

    {
        const auto grammar = 1 * terminal('a');

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
            assert(context.mark() == std::next(context.get_input().begin()));
        }
    }

    {
        const auto grammar = 2 * terminal('a');

        {
            const std::string input = "a";
            Context context(input);
            bool ok = grammar(context);
            assert(!ok);
            assert(context.mark() == context.get_input().begin());
        }

        {
            const std::string input = "aa";
            Context context(input);
            bool ok = grammar(context);
            assert(ok);
            assert(context.mark() == context.get_input().end());
        }
    }
}

static void testNotExpr() {
    const auto grammar = !terminal('a');

    {
        const std::string input = "b";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().begin());
    }

    {
        const std::string input = "a";
        Context context(input);
        bool ok = grammar(context);
        assert(!ok);
        assert(context.mark() == context.get_input().begin());
    }
}

static void testOptionalExpr() {
    const auto grammar = -terminal('a');

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
        assert(context.mark() == context.get_input().begin());
    }
}

static void testNonTerminalExpr() {
    const Rule<std::string::value_type> grammar = 'a' >> (grammar | 'b')
                      | 'b' >> ('a' | grammar);

    {
        const std::string input = "ab";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().end());
    }

    {
        const std::string input = "aab";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().end());
    }

    {
        const std::string input = "aaab";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().end());
    }

    {
        const std::string input = "b";
        Context context(input);
        bool ok = grammar(context);
        assert(!ok);
        assert(context.mark() == context.get_input().begin());
    }
}

static void testSequenceExpr() {
    {
        const auto grammar = terminal('a') >> 'b' >> 'c';

        {
            const std::string input = "abc";
            Context context(input);
            bool ok = grammar(context);
            assert(ok);
            assert(context.mark() == context.get_input().end());
        }

        {
            const std::string input = "dabc";
            Context context(input);
            bool ok = grammar(context);
            assert(!ok);
            assert(context.mark() == context.get_input().begin());
        }

        {
            const std::string input = "adbc";
            Context context(input);
            bool ok = grammar(context);
            assert(!ok);
            assert(context.mark() == context.get_input().begin());
        }
    }
}

static void testTerminalRangeExpr() {
    const auto grammar = terminal('0', '9');

    {
        const std::string input = "0";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().end());
    }

    {
        const std::string input = "b";
        Context context(input);
        bool ok = grammar(context);
        assert(!ok);
        assert(context.mark() == context.get_input().begin());
    }
}

static void testTerminalSetExpr() {
    const auto grammar = terminal(std::set<char>{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'});

    {
        const std::string input = "0";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().end());
    }

    {
        const std::string input = "5";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().end());
    }

    {
        const std::string input = "9";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().end());
    }

    {
        const std::string input = "b";
        Context context(input);
        bool ok = grammar(context);
        assert(!ok);
        assert(context.mark() == context.get_input().begin());
    }
}

static void testTerminalSeqExpr() {
    const auto grammar = terminalSeq("int");

    {
        const std::string input = "int";
        Context context(input);
        bool ok = grammar(context);
        assert(ok);
        assert(context.mark() == context.get_input().end());
    }

    {
        const std::string input = "b";
        Context context(input);
        bool ok = grammar(context);
        assert(!ok);
        assert(context.mark() == context.get_input().begin());
    }
}


int main(int argc, char* argv[]){
    testAndExpr();
    testAlternationExpr();
    testZeroOrMoreExpr();
    testOneOrMoreExpr();
    testNTimesExpr();
    testNotExpr();
    testOptionalExpr();
    testNonTerminalExpr();
    testSequenceExpr();
    testTerminalSetExpr();
    testTerminalSeqExpr();
    return 0;
}
