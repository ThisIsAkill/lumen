#pragma once

#include <QString>

// Scores how well query matches text, case-insensitively:
//   100  exact match
//    80  text starts with the query
//    60  every query word starts a word of the text ("vs co" → "Visual Studio Code")
//    40  query is a substring
//    20  query's letters appear in order ("gwvw" → "Gwenview")
//    10  a word with a typo: one letter wrong, missing, extra or swapped
//        (two for words of 8+ letters), e.g. "firfox" → "Firefox"
//     0  no match
// Ties are broken slightly in favour of shorter texts.
qreal fuzzyScore(const QString &query, const QString &text);
