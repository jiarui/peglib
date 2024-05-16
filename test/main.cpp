#include "peglib.h"
#include <string>
#include <iostream>
#include <sstream>
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

static void testMatchExpr() {
    const auto grammar = terminal('a') == 1;

    {
        const std::string input = "a";
        Context context(input);
        bool ok = grammar(context);
        const auto matches = context.matches();
        assert(ok);
        assert(context.mark() == context.get_input().end());
        assert(matches.size() == 1);
        assert(matches[0].id() == 1);
    }

    {
        const std::string input = "b";
        Context context(input);
        bool ok = grammar(context);
        const auto matches = context.matches();
        assert(!ok);
        assert(context.mark() == context.get_input().begin());
        assert(matches.size() == 0);
    }
}

static void testRecursion() {
    const Rule<std::string::value_type> r = 'x' >> r >> 'b'
                   | 'a';

    {
        const std::string input = "a";
        Context context(input);
        bool ok = r(context);
        assert(ok);
        assert(context.mark() == context.get_input().end());
    }

    {
        const std::string input = "xab";
        Context context(input);
        bool ok = r(context);
        assert(ok);
        assert(context.mark() == context.get_input().end());
    }

    {
        const std::string input = "xxabb";
        Context context(input);
        bool ok = r(context);
        assert(ok);
        assert(context.mark() == context.get_input().end());
    }
}

#if 1

extern Rule<std::string::value_type> add;


static auto digit = terminal('0', '9');


static auto integer = +digit;


static auto num = integer
                | '(' >> add >> ')';


static Rule<std::string::value_type> mul = (mul >> '*' >> num)
                  | (mul >> '/' >> num)
                  | num;


Rule<std::string::value_type> add = (add >> '+' >> mul)
                  | (add >> '-' >> mul)
                  | mul;

template<typename MatchType>
static int eval(const MatchType& m) {
    if (m.id() == "add") {
        return eval(m.children()[0]) + eval(m.children()[1]);
    }
    if (m.id() == "sub") {
        return eval(m.children()[0]) - eval(m.children()[1]);
    }
    if (m.id() == "mul") {
        return eval(m.children()[0]) * eval(m.children()[1]);
    }
    if (m.id() == "div") {
        return eval(m.children()[0]) / eval(m.children()[1]);
    }
    if (m.id() == "int") {
        std::stringstream stream;
        stream << m.content();
        int v;
        stream >> v;
        return v;
    }
    throw std::logic_error("Invalid match id");
}

static void testLeftRecursion() {
    {
        Rule<std::string::value_type> r = (r >> 'b')
                 | (r >> 'c')
                 | terminal('a')
                 | terminal('d');

        // {
        //     const std::string input = "a";
        //     Context context(input);
        //     bool ok = r(context);
        //     assert(ok);
        //     assert(context.ended());
        // }

        {
            const std::string input = "ab";
            Context context(input);
            bool ok = r(context);
            assert(ok);
            assert(context.ended());
        }

        {
            const std::string input = "abc";
            Context context(input);
            bool ok = r(context);
            assert(ok);
            assert(context.ended());
        }

        {
            const std::string input = "acb";
            Context context(input);
            bool ok = r(context);
            assert(ok);
            assert(context.ended());
        }

        {
            const std::string input = "abcb";
            Context context(input);
            bool ok = r(context);
            assert(ok);
            assert(context.ended());
        }

        {
            const std::string input = "acbc";
            Context context(input);
            bool ok = r(context);
            assert(ok);
            assert(context.ended());
        }

        {
            const std::string input = "aa";
            Context context(input);
            bool ok = r(context);
            assert(ok);
            assert(context.ended());
        }

        {
            const std::string input = "aba";
            Context context(input);
            bool ok = r(context);
            assert(ok);
            assert(!context.ended());
        }

        {
            const std::string input = "aca";
            Context context(input);
            bool ok = r(context);
            assert(ok);
            assert(!context.ended());
        }

        {
            const std::string input = "b";
            Context context(input);
            bool ok = r(context);
            assert(ok);
            assert(!context.ended());
        }

        {
            const std::string input = "c";
            Context context(input);
            bool ok = r(context);
            assert(ok);
            assert(!context.ended());
        }

        {
            const std::string input = "ba";
            Context context(input);
            bool ok = r(context);
            assert(ok);
            assert(!context.ended());
        }

        {
            const std::string input = "ca";
            Context context(input);
            bool ok = r(context);
            assert(ok);
            assert(!context.ended());
        }

        {
            const std::string input = "ad";
            Context context(input);
            bool ok = r(context);
            assert(ok);
            assert(!context.ended());
        }

        {
            const std::string input = "abd";
            Context context(input);
            bool ok = r(context);
            assert(ok);
            assert(!context.ended());
        }
    }

    {
        std::string input = "1";
        Context context(input);
        const bool ok = add(context);
        assert(ok);
    }

    {
        std::string input = "1+2";
        Context context(input);
        const bool ok = add(context);
        assert(ok);
    }

    {
        std::string input = "1+2*3";
        Context context(input);
        const bool ok = add(context);
        assert(ok);
    }

    {
        std::string input = "1*2+3";
        Context context(input);
        const bool ok = add(context);
        assert(ok);
    }

    {
        std::string input = "(1+2)*3";
        Context context(input);
        const bool ok = add(context);
        assert(ok);
    }

    {
        std::string input = "1*(2+3)";
        Context context(input);
        const bool ok = add(context);
        assert(ok);
    }

    {
        std::string input = "(1*(2+3))*4";
        Context context(input);
        const bool ok = add(context);
        assert(ok);
    }
}

#endif

int main(int argc, char* argv[]){
    // testAndExpr();
    // testAlternationExpr();
    // testZeroOrMoreExpr();
    // testOneOrMoreExpr();
    // testNTimesExpr();
    // testNotExpr();
    // testOptionalExpr();
    // testNonTerminalExpr();
    // testSequenceExpr();
    // testTerminalSetExpr();
    // testTerminalSeqExpr();
    // testMatchExpr();
    // testRecursion();
    testLeftRecursion();
    return 0;
}
