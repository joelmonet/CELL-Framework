#include <cell/json.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>
namespace j = cell::json;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Work> void rejected(Work work) { bool failed = false; try { work(); } catch (const j::Error&) { failed = true; } check(failed, "Invalid or over-budget JSON was accepted"); }
void corpus() {
    check(j::write(j::Value(UINT64_MAX))=="18446744073709551615"&&j::write(j::Value(INT64_MIN))=="-9223372036854775808","Exact JSON integers lost precision");
    check(*j::read("18446744073709551615").unsignedInteger()==UINT64_MAX&&*j::read("-9223372036854775808").signedInteger()==INT64_MIN,"Parsed JSON integer identity lost precision");
    check(j::write(j::Value("native"))=="\"native\"","Native JSON string construction became ambiguous");
    const auto document = j::read(R"({"model":{"type":"fir","weights":[0.5,-2e-3,1E+2]},"flag":true,"empty":null,"text":"\ud834\udd1e\n\u0000"})");
    check(document.isObject() && document["model"]["type"].text() == "fir", "JSON lookup changed an owned string");
    check(document["model"]["weights"].array()->size() == 3 && *(*document["model"]["weights"].array())[1].number() == -.002, "JSON array/number changed");
    check(document["missing"].isNull() && document["empty"].isNull() && *document["flag"].boolean(), "JSON missing/null/bool coercion changed");
    const auto text = *document["text"].string(); check(text.size() == 6 && text.back() == '\0', "JSON surrogate or NUL decoding changed");
    const auto encoded = j::write(document); check(j::write(j::read(encoded)) == encoded, "JSON canonical round trip changed");
    check(*j::read("\xef\xbb\xbf -0.0").number() == 0 && std::signbit(*j::read("-0.0").number()), "JSON BOM or negative zero changed");
    for (const std::string input : {"", " ", "01", "-01", "+1", "1.", ".1", "1e", "1e+", "--1", "NaN", "Infinity", "1e9999", "true false", "[1,]", "{\"x\":1,}", "{\"x\" 1}", "{\"x\":1,\"\\u0078\":2}", "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0041\"", "\"\\x20\"", "\"line\nend\""})
        rejected([&] { j::read(input); });
    for (const std::string input : {std::string("\"\xc0\xaf\""), std::string("\"\xed\xa0\x80\""), std::string("\"\xf4\x90\x80\x80\""), std::string("\"\xe2\x82\"")})
        rejected([&] { j::read(input); });
    rejected([] { j::write(j::Value(std::numeric_limits<double>::infinity())); });
    rejected([] { j::write(j::Value(std::string("\x80"))); });
}
void budgets() {
    j::Limits limits; limits.inputBytes = 3;
    check(*j::read("123", limits).number() == 123, "Exact JSON byte budget failed"); rejected([&] { j::read("1234", limits); });
    limits = {}; limits.nodes = 3; j::read("[1,2]", limits); rejected([&] { j::read("[1,2,3]", limits); });
    limits = {}; limits.depth = 2; j::read("[[0]]", limits); rejected([&] { j::read("[[[0]]]", limits); });
    limits = {}; limits.textBytes = 2; j::read("{\"a\":\"b\"}", limits); rejected([&] { j::read("{\"a\":\"bc\"}", limits); });
    limits = {}; limits.depth = 257; rejected([&] { j::read("[]", limits); });
    const auto value = j::read("{\"a\":\"b\"}"); limits = {}; limits.inputBytes = 8; rejected([&] { j::write(value, limits); });
    limits = {}; limits.textBytes = 1; rejected([&] { j::write(value, limits); });
    limits = {}; limits.nodes = 1; rejected([&] { j::write(value, limits); });
    limits = {}; limits.depth = 1; rejected([&] { j::write(j::read("[[0]]"), limits); });
    limits = {}; limits.inputBytes = 8; check(j::write(j::Value(std::string(1, '\0')), limits) == "\"\\u0000\"", "JSON escaped output exact byte budget changed");
    limits.inputBytes = 7; rejected([&] { j::write(j::Value(std::string(1, '\0')), limits); });
}
void recursiveEquality() {
    const auto document = j::read(R"({"rows":[{"value":4294967297,"name":"Drive"}],"ready":true})");
    auto copy = document;
    check(copy == document, "Copied JSON object differs from its source");
    copy = j::read(R"({"ready":true,"rows":[{"name":"Drive","value":4294967297}]})");
    check(copy == document, "JSON object order affected recursive equality");
    auto object = *copy.object();
    auto rows = *object.at("rows").array();
    auto row = *rows.front().object();
    row.insert_or_assign("value", j::Value(4294967298ULL));
    rows.front() = j::Value(std::move(row));
    object.insert_or_assign("rows", j::Value(std::move(rows)));
    check(j::Value(std::move(object)) != document, "Recursive equality missed a changed JSON integer");
}
}
int main() { try { corpus(); budgets(); recursiveEquality(); std::cout << "PASS: bounded JSON, strict grammar, Unicode and canonical round trips\n"; } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
