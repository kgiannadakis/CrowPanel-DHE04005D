#include "graphics/mcui/screens/NodeSearch.h"

#include <cassert>
#include <cstdio>
#include <random>
#include <string>

using mcui::node_search::bounded_text;
using mcui::node_search::contains;

int main()
{
    assert(contains("Heltec Berlin", ""));
    assert(contains("Heltec Berlin", "BERlin"));
    assert(contains("Heltec Berlin", "heltec"));
    assert(!contains("Heltec Berlin", "Berlin2"));
    assert(!contains("", "node"));
    assert(contains("!ACaae48c", "acaae"));
    assert(contains(u8"M\u00fcnchen S\u00fcd", u8"M\u00dcNCHEN"));
    assert(contains(u8"Stra\u00dfe", u8"STRA\u1e9eE"));
    assert(contains(u8"\u0391\u03b8\u03ae\u03bd\u03b1", u8"\u0391\u0398\u0389\u039d\u0391"));
    assert(contains(u8"\u03ba\u03cc\u03c3\u03bc\u03bf\u03c2", u8"\u039a\u038c\u03a3\u039c\u039f\u03a3"));
    assert(contains(u8"\U0001f600 Heltec", u8"\U0001f600"));
    assert(!contains(u8"\U0001f600", "\x9f\x98"));
    assert(!contains(u8"M\u00fcnchen", "Munchen")); // Accents aren't discarded.
    const char fixed[] = {'n', 'o', 'd', 'e'};
    assert(contains(bounded_text(fixed), "NODE"));
    const char terminated[] = {'n', '\0', 'x'};
    assert(bounded_text(terminated).size() == 1);
    assert(!contains(bounded_text(terminated), "x"));
    assert(contains("bad\xc3" "name", "NAME"));
    assert(!contains("\xc0\xaf", "/"));
    assert(!contains("\xed\xa0\x80", "?"));

    // Exercise every malformed-byte boundary under UBSan, plus bounded non-NUL names.
    std::mt19937 rng(42);
    for (unsigned iteration = 0; iteration < 20000; ++iteration) {
        std::string name(rng() % 80, '\0');
        for (char &byte : name)
            byte = static_cast<char>(rng());
        std::string query(rng() % 40, '\0');
        for (char &byte : query)
            byte = static_cast<char>(rng());
        (void)contains(name, query);
        assert(contains(name, name));
        assert(contains(name, ""));
    }
    std::puts("Node search tests passed (Latin/German/Greek, UTF-8 boundaries, 20000 randomized inputs).");
}
