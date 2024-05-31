#include <boost/test/unit_test.hpp>
#include "peglib/FileSource.h"
#include "peglib.h"
#include <fstream>
using namespace peg;
BOOST_AUTO_TEST_CASE(test_file_io){//, * boost::unit_test::disabled()) {
    std::string file_name = "/home/coaxmetal/Workspace/peglib/LICENSE";
    auto context = from_file<char>(file_name, 4096);
    auto fs = std::fstream(file_name);

    std::string file((std::istreambuf_iterator<char>(fs)),
                 std::istreambuf_iterator<char>());
    context.mark();
    auto c = file.begin();
    while(!context.ended()){
        BOOST_CHECK_EQUAL(*c, *context.mark());
        ++c;
        context.next();
    }
}