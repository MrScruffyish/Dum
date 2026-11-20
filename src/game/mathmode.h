// Dum - hidden F7 mode: monsters become math problems.
// The question is derived from the monster's id/definition/depth, so the client can draw it
// and the server can check the answer without sending the question over the network.
#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mathmode {

struct Rational {
    int num = 0, den = 1;
    Rational() = default;
    Rational(int n, int d = 1) { set(n, d); }
    void set(int n, int d) {
        if (d == 0) { num = 0; den = 1; return; }
        if (d < 0) { n = -n; d = -d; }
        num = n;
        den = d;
    }
    // Cross-multiply: 5/10 == 1/2, no need to reduce anything.
    bool operator==(const Rational& o) const { return (long long)num * o.den == (long long)o.num * den; }
    bool operator!=(const Rational& o) const { return !(*this == o); }
};

inline std::string fraction(const Rational& r) {
    if (r.den == 1) return std::to_string(r.num);
    return std::to_string(r.num) + "/" + std::to_string(r.den);
}

namespace detail {

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    int range(int lo, int hi) { return lo + (int)(next() % (uint32_t)(hi - lo + 1)); }
};

inline std::string termText(int a, char var) {
    if (a == 0) return std::string();
    std::string t = a < 0 ? "-" : "";
    int m = a < 0 ? -a : a;
    if (!(m == 1 && var)) t += std::to_string(m);
    if (var) t += var;
    return t;
}

// "-2x + 3 - y" style formatting for a list of (coefficient, variable) pairs; var 0 = constant.
inline std::string lin(const std::vector<std::pair<int, char>>& terms) {
    std::string s;
    for (auto t : terms) {
        if (t.first == 0) continue;
        std::string part = termText(t.first, t.second);
        if (s.empty()) s = part;
        else s += (t.first > 0 ? " + " : " - ") + (t.first > 0 ? part : part.substr(1));
    }
    return s.empty() ? "0" : s;
}

}  // namespace detail

struct Question {
    std::string text;                 // what floats where the monster used to be
    std::string prompt;               // what the answer box asks for
    std::vector<Rational> answers;    // one per variable (x, or x and y for bosses)
    bool boss = false;
    bool matches(const std::string& input) const;
};

inline Question question(uint32_t monsterId, int def, int depth, bool boss) {
    using detail::Rng;
    using detail::lin;
    Rng rng(monsterId * 2654435761u ^ (uint32_t)(def + 1) * 2246822519u ^ (uint32_t)(depth + 7) * 3266489917u);
    Question q;
    q.boss = boss;

    if (boss) {
        int a = 1, b = 0, c = 0, d = 1, det = 1;
        for (int tries = 0; tries < 32; tries++) {
            a = rng.range(-4, 4); b = rng.range(-4, 4);
            c = rng.range(-4, 4); d = rng.range(-4, 4);
            det = a * d - b * c;
            if (det != 0 && (a || b) && (c || d)) break;
        }
        if (det == 0 || ((a == 0 && b == 0) || (c == 0 && d == 0))) {
            a = 1; b = 0; c = 0; d = 1;
        }
        int den = rng.range(1, 3);          // 1 = integer answers, 2-3 = fractions
        int nx = rng.range(-6, 7), ny = rng.range(-6, 7);
        if (den > 1 && nx % den == 0) nx += 1;
        if (den > 1 && ny % den == 0) ny += 1;
        int rhs1 = a * nx + b * ny;         // the equations are scaled by den so the RHS stays whole
        int rhs2 = c * nx + d * ny;
        q.text = lin({{den * a, 'x'}, {den * b, 'y'}}) + " = " + std::to_string(rhs1) + ", " +
                 lin({{den * c, 'x'}, {den * d, 'y'}}) + " = " + std::to_string(rhs2);
        q.prompt = "x =     y = ";
        q.answers = {Rational(nx, den), Rational(ny, den)};
        return q;
    }

    int tier = depth <= 2 ? 0 : (depth <= 5 ? 1 : 2);
    if (def > 24 && tier < 2) tier++;     // nastier monsters on the same floor get more terms
    if (def > 48 && tier < 2) tier++;
    int den = tier == 2 ? rng.range(2, 4) : 1;
    int num = rng.range(-9, 10);
    if (den > 1 && num % den == 0) num += (num < 0 ? -1 : 1);
    int a = den * (tier == 0 ? 1 : rng.range(2, 4));
    int b = rng.range(-7, 8);
    int c = tier >= 1 ? rng.range(-5, 6) : 0;
    int rhs = (a / den) * num + b + c;

    std::vector<std::pair<int, char>> terms = {{a, 'x'}};
    if (b != 0) terms.push_back({b, 0});
    if (c != 0) terms.push_back({c, 0});
    q.text = lin(terms) + " = " + std::to_string(rhs);
    q.prompt = "x = ";
    q.answers = {Rational(num, den)};
    return q;
}

inline bool parseOne(const std::string& s, size_t& i, Rational& out) {
    int sign = 1;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        if (s[i] == '-') sign = -1;
        i++;
    }
    if (i >= s.size() || !std::isdigit((unsigned char)s[i])) return false;
    long long n = 0;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) n = n * 10 + (s[i++] - '0');

    size_t save = i;
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    int den = 1;
    if (i < s.size() && s[i] == '/') {
        i++;
        while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
        int dsign = 1;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) { if (s[i] == '-') dsign = -1; i++; }
        if (i < s.size() && std::isdigit((unsigned char)s[i])) {
            long long d = 0;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) d = d * 10 + (s[i++] - '0');
            if (d == 0) return false;
            den = (int)(d * dsign);
        } else {
            i = save;
        }
    } else {
        i = save;
    }
    out.set((int)(sign * n), den);
    return true;
}

// Accepts "5", "5/10", "1 / 2", "x=1/2 y=3", "3,4" ...
inline std::vector<Rational> parseAnswer(const std::string& s) {
    std::vector<Rational> v;
    size_t i = 0;
    while (i < s.size() && v.size() < 4) {
        while (i < s.size() && !std::isdigit((unsigned char)s[i]) && s[i] != '-' && s[i] != '+') i++;
        if (i >= s.size()) break;
        Rational r;
        if (parseOne(s, i, r)) v.push_back(r);
        else i++;
    }
    return v;
}

inline bool Question::matches(const std::string& input) const {
    std::vector<Rational> v = parseAnswer(input);
    if (v.size() != answers.size()) return false;
    for (size_t i = 0; i < v.size(); i++) if (v[i] != answers[i]) return false;
    return true;
}

}  // namespace mathmode
