#pragma once

// PEG_RULE macros removed in Phase 2 (Grammar API).
// Use Grammar::operator[] instead — rules are auto-named from the map key:
//
//   Grammar<> g;
//   g["numeral"] = terminal('0', '9') >> +terminal('0', '9');
//   g["expr"] = g["expr"] >> '+' >> g["numeral"] | g["numeral"];
