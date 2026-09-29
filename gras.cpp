#include <algorithm>
#include <array>
#include <cassert>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#ifndef CPPHTTPLIB_PAYLOAD_MAX_LENGTH
#define CPPHTTPLIB_PAYLOAD_MAX_LENGTH (static_cast<std::size_t>(1024) * 1024 * 1024)
#endif
#ifndef CPPHTTPLIB_FORM_URL_ENCODED_PAYLOAD_MAX_LENGTH
#define CPPHTTPLIB_FORM_URL_ENCODED_PAYLOAD_MAX_LENGTH (static_cast<std::size_t>(1024) * 1024 * 1024)
#endif
#ifndef CPPHTTPLIB_RECV_BUFSIZ
#define CPPHTTPLIB_RECV_BUFSIZ (static_cast<std::size_t>(1024) * 1024)
#endif
#include "httplib.h"

namespace gras {

    using NameId = uint32_t;
    using NodeId = uint32_t;
    using TypeBits = uint64_t;
    static constexpr NameId NoName = std::numeric_limits<NameId>::max();
    static constexpr NodeId NoNode = std::numeric_limits<NodeId>::max();

    // These are deliberately pre-seeded in Interner so the interpreter can switch
    // directly on NameId rather than storing function pointers/lambdas.
    enum FixedName : NameId {
        N_Impl_ncast = 0,
        N_Impl_fcast,
        N_Impl_nadd,
        N_Impl_nsub,
        N_Impl_nmul,
        N_Impl_ndiv,
        N_Impl_npow,
        N_Impl_nmod,
        N_Impl_nmin,
        N_Impl_nmax,
        N_Impl_fadd,
        N_Impl_fsub,
        N_Impl_fmul,
        N_Impl_fdiv,
        N_Impl_fpow,
        N_Impl_fmin,
        N_Impl_fmax,
        N_Impl_flog,
        N_Impl_scat,
        N_Impl_nget,
        N_Impl_fget,
        N_Impl_nslice,
        N_Impl_fslice,
        N_Impl_sslice,
        N_Impl_sarray,
        N_Impl_narray,
        N_Impl_farray,
        N_Impl_aarray,
        N_Impl_acat,
        N_Impl_map,
        N_Impl_reduce,
        N_Impl_loadsarray,
        N_Impl_loadstring,
        N_Impl_savesarray,
        N_Impl_savestring,
        N_Impl_ssplit,
        N_Impl_ston,
        N_Impl_stof,
        N_Impl_ntos,
        N_Impl_ftos,
        N_Impl_Nat,
        N_Impl_Int,
        N_Impl_Real,
        N_Impl_Float,
        N_Impl_String,
        N_Impl_nat,
        N_Impl_int,
        N_Impl_real,
        N_Impl_float,
        N_Impl_string,
        N_fixed_count
    };

    struct Interner {
        std::unordered_map<std::string, NameId> ids;
        std::vector<std::string> strings;

        Interner() {
            static constexpr std::array<std::string_view, N_fixed_count> fixed = {
                "Impl::ncast", "Impl::fcast",
                "Impl::nadd", "Impl::nsub", "Impl::nmul", "Impl::ndiv", "Impl::npow", "Impl::nmod", "Impl::nmin", "Impl::nmax",
                "Impl::fadd", "Impl::fsub", "Impl::fmul", "Impl::fdiv", "Impl::fpow", "Impl::fmin", "Impl::fmax", "Impl::flog",
                "Impl::scat", "Impl::nget", "Impl::fget", "Impl::nslice", "Impl::fslice", "Impl::sslice",
                "Impl::sarray", "Impl::narray", "Impl::farray", "Impl::aarray", "Impl::acat", "Impl::map", "Impl::reduce",
                "Impl::loadsarray", "Impl::loadstring", "Impl::savesarray", "Impl::savestring", "Impl::ssplit",
                "Impl::ston", "Impl::stof", "Impl::ntos", "Impl::ftos",
                "Impl::Nat", "Impl::Int", "Impl::Real", "Impl::Float", "Impl::String",
                "Impl::nat", "Impl::int", "Impl::real", "Impl::float", "Impl::string"
            };
            for (NameId i = 0; i < fixed.size(); ++i) {
                NameId got = intern(fixed[i]);
                if (got != i) throw std::logic_error("fixed interner seed mismatch");
            }
        }

        NameId intern(std::string_view s) {
            auto it = ids.find(std::string(s));
            if (it != ids.end()) return it->second;
            NameId id = static_cast<NameId>(strings.size());
            strings.emplace_back(s);
            ids.emplace(strings.back(), id);
            return id;
        }
        std::string_view str(NameId id) const {
            if (id == NoName) return {};
            return strings.at(id);
        }
    };

    struct Pos {
        uint32_t offset = 0, line = 1, column = 1;
    };
    struct Span {
        Pos start{}, end{};
    };

    struct Error : std::runtime_error {
        std::string file;
        Span span;
        std::string graph_context;
        Error(std::string f, Span s, std::string message, std::string graph = {})
        : std::runtime_error(std::move(message)), file(std::move(f)), span(s), graph_context(std::move(graph)) {}
    };

    namespace ansi {
        static constexpr std::string_view RESET = "\033[0m";
        static constexpr std::string_view BOLD = "\033[1m";
        static constexpr std::string_view RED = "\033[31m";
        static constexpr std::string_view GREEN = "\033[32m";
        static constexpr std::string_view YELLOW = "\033[33m";
        static constexpr std::string_view PURPLE = "\033[35m";
        static constexpr std::string_view GRAY = "\033[90m";
        static constexpr std::string_view CYAN = "\033[36m";
        static constexpr std::string_view RED_UL = "\033[4;31m";
    }

    static std::string color(std::string_view s, std::string_view c) {
        return std::string(c) + std::string(s) + std::string(ansi::RESET);
    }
    static std::string kw(std::string_view s) {
        return color(s, ansi::PURPLE);
    }
    static std::string green(std::string_view s) {
        return color(s, ansi::GREEN);
    }
    static std::string yellow(std::string_view s) {
        return color(s, ansi::YELLOW);
    }
    static std::string gray(std::string_view s) {
        return color(s, ansi::GRAY);
    }
    static std::string cyan(std::string_view s) {
        return color(s, ansi::CYAN);
    }

    static std::string_view literal_text(std::string_view s) {
        size_t p = s.rfind("::");
        return p == std::string_view::npos ? s : s.substr(p + 2);
    }
    static bool is_string_literal(std::string_view s) {
        s = literal_text(s);
        return s.size() >= 2 && s.front() == '"' && s.back() == '"';
    }
    static bool is_numeric_literal(std::string_view s) {
        s = literal_text(s);
        size_t i = 0;
        auto digits = [&]() {
            size_t begin = i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
            return i > begin;
        };
        bool mantissa = false;
        if (i < s.size() && s[i] == '.') {
            ++i;
            mantissa = digits();
        } else if (digits()) {
            mantissa = true;
            if (i < s.size() && s[i] == '.') {
                ++i;
                if (!digits()) return false;
            }
        }
        if (!mantissa) return false;
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            if (!digits()) return false;
        }
        return i == s.size();
    }
    static bool is_literal(std::string_view s) {
        return is_string_literal(s) || is_numeric_literal(s);
    }
    static NameId literal_base_type(Interner& names, NameId literal) {
        std::string_view raw = literal_text(names.str(literal));
        if (is_string_literal(raw)) return names.intern("Impl::string");
        if (is_numeric_literal(raw)) {
            return names.intern(raw.find('.') == std::string_view::npos ? "Impl::nat" : "Impl::float");
        }
        return NoName;
    }

    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp <= 0x7f) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }
    static int hex_digit(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    static std::string unquote_string(std::string_view raw) {
        raw = literal_text(raw);
        if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') return std::string(raw);
        std::string out;
        for (size_t i = 1; i + 1 < raw.size(); ++i) {
            char c = raw[i];
            if (c != '\\' || i + 1 >= raw.size() - 1) {
                out.push_back(c);
                continue;
            }
            char e = raw[++i];
            switch (e) {
                case 'n': out.push_back('\n');
                break;
                case 'r': out.push_back('\r');
                break;
                case 't': out.push_back('\t');
                break;
                case 'b': out.push_back('\b');
                break;
                case 'f': out.push_back('\f');
                break;
                case '\\': out.push_back('\\');
                break;
                case '"': out.push_back('"');
                break;
                case '/': out.push_back('/');
                break;
                case 'u': {
                    if (i + 4 >= raw.size()) {
                        out += "\\u";
                        break;
                    }
                    uint32_t cp = 0;
                    bool ok = true;
                    for (int k = 0; k < 4; ++k) {
                        int h = hex_digit(raw[i + 1 + k]);
                        if (h < 0) {
                            ok = false;
                            break;
                        } cp = (cp << 4) | static_cast<uint32_t>(h);
                    }
                    if (!ok) {
                        out += "\\u";
                        break;
                    }
                    i += 4;
                    if (cp >= 0xd800 && cp <= 0xdbff && i + 6 < raw.size() && raw[i+1] == '\\' && raw[i+2] == 'u') {
                        uint32_t lo = 0;
                        bool lok = true;
                        for (int k = 0; k < 4; ++k) {
                            int h = hex_digit(raw[i + 3 + k]);
                            if (h < 0) {
                                lok = false;
                                break;
                            } lo = (lo << 4) | static_cast<uint32_t>(h);
                        }
                        if (lok && lo >= 0xdc00 && lo <= 0xdfff) {
                            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                            i += 6;
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: out.push_back(e);
                break;
            }
        }
        return out;
    }

    static std::string typefmt(std::string_view s) {
        size_t p = s.rfind("::");
        if (p == std::string_view::npos) return is_literal(s) ? yellow(s) : green(s);
        auto u = s.substr(0, p + 2);
        auto n = s.substr(p + 2);
        return gray(u) + (is_literal(n) ? yellow(n) : green(n));
    }

    static std::string join(const std::vector<std::string>& xs, std::string_view sep) {
        std::string out;
        for (size_t i = 0; i < xs.size(); ++i) {
            if (i) out += sep;
            out += xs[i];
        }
        return out;
    }

    enum class Kind : uint8_t {
        Name, String, Rel, Colon, Pipe, Universe, Uses, Def, Return, LPar, RPar,
        Where, Reduce, Do, Run, Import, All, Close, Eol, Comma, Eof
    };
    struct Token {
        Kind kind{};
        NameId text = NoName;
        Span span{};
    };

    static std::string kind_name(Kind k) {
        switch (k) {
            case Kind::Name: return "name";
            case Kind::String: return "string";
            case Kind::Rel: return "relation";
            case Kind::Colon: return "colon";
            case Kind::Pipe: return "pipe";
            case Kind::Universe: return "universe";
            case Kind::Uses: return "uses";
            case Kind::Def: return "def";
            case Kind::Return: return "return";
            case Kind::LPar: return "lpar";
            case Kind::RPar: return "rpar";
            case Kind::Where: return "where";
            case Kind::Reduce: return "reduce";
            case Kind::Do: return "do";
            case Kind::Run: return "run";
            case Kind::Import: return "import";
            case Kind::All: return "all";
            case Kind::Close: return "close";
            case Kind::Eol: return "eol";
            case Kind::Comma: return "comma";
            case Kind::Eof: return "eof";
        }
        return "?";
    }

    struct Lexer {
        std::string file;
        std::string source;
        Interner& names;
        size_t i = 0;
        uint32_t line = 1, col = 1;

        Lexer(std::string f, std::string s, Interner& n) : file(std::move(f)), source(std::move(s)), names(n) {}
        Pos pos() const { return {static_cast<uint32_t>(i), line, col};
        }
        bool eof() const { return i >= source.size();
        }
        char peek(size_t n = 0) const { return i + n < source.size() ? source[i + n] : '\0';
        }
        char advance() {
            char c = source[i++];
            if (c == '\n') {
                ++line;
                col = 1;
            } else ++col;
            return c;
        }
        Token make(Kind k, std::string_view s, Pos start) {
            return {k, names.intern(s), {start, pos()}};
        }
        static bool relchar(char c) {
            constexpr std::string_view chars = "~=<>!+-*/%^&@#$?\\()";
            return chars.find(c) != std::string_view::npos;
        }
        void skip() {
            for (;;) {
                while (!eof() && (peek() == ' ' || peek() == '\t' || peek() == '\r')) advance();
                if (peek() != '/' || peek(1) != '/') return;
                while (!eof() && peek() != '\n') advance();
            }
        }
        Kind keyword(std::string_view s) const {
            static constexpr std::array<std::pair<std::string_view, Kind>, 11> ks = {{
                    {"universe", Kind::Universe}, {"uses", Kind::Uses}, {"def", Kind::Def},
                    {"return", Kind::Return}, {"where", Kind::Where}, {"reduce", Kind::Reduce},
                    {"do", Kind::Do}, {"run", Kind::Run}, {"import", Kind::Import}, {"all", Kind::All}, {"close", Kind::Close}
            }};
            for (auto [text, kind] : ks) if (s == text) return kind;
            return Kind::Name;
        }
        Token string_token() {
            Pos start = pos();
            std::string text;
            text += advance();
            while (!eof()) {
                char c = advance();
                text += c;
                if (c == '\\') {
                    if (eof()) break;
                    text += advance();
                }
                else if (c == '"') return make(Kind::String, text, start);
            }
            throw Error(file, {start, pos()}, "unterminated string literal");
        }
        Token name_token() {
            Pos start = pos();
            std::string text;
            while (!eof()) {
                char c = peek();
                if (std::isspace(static_cast<unsigned char>(c)) || c == '|' || c == ',' || c == '"' || relchar(c)) break;
                if (c == ':') {
                    if (peek(1) == ':') {
                        text += advance();
                        text += advance();
                        continue;
                    }
                    break;
                }
                text += advance();
            }
            if (text.empty()) throw Error(file, {start, pos()}, "unexpected character '" + std::string(1, peek()) + "'");
            return make(keyword(text), text, start);
        }
        Token relation_token() {
            Pos start = pos();
            std::string text;
            while (!eof() && !std::isspace(static_cast<unsigned char>(peek())) && peek() != ':' && peek() != '|' && peek() != '"' && relchar(peek())) {
                if (peek() == '/' && peek(1) == '/') break;
                text += advance();
            }
            return make(Kind::Rel, text, start);
        }
        Token next() {
            skip();
            Pos start = pos();
            if (eof()) return {Kind::Eof, names.intern(""), {start, start}};
            if (peek() == '\n') {
                advance();
                return make(Kind::Eol, "\n", start);
            }
            if (peek() == ',') {
                advance();
                return make(Kind::Comma, ",", start);
            }
            if (peek() == '"') return string_token();
            if (peek() == ':') {
                advance();
                return make(Kind::Colon, ":", start);
            }
            if (peek() == '|') {
                advance();
                return make(Kind::Pipe, "|", start);
            }
            if (peek() == '(') {
                advance();
                return make(Kind::LPar, "(", start);
            }
            if (peek() == ')') {
                advance();
                return make(Kind::RPar, ")", start);
            }
            if (relchar(peek())) return relation_token();
            return name_token();
        }
        struct Tokenization {
            std::vector<Token> tokens;
            std::optional<Error> error;
        };

        Tokenization tokenize_recovering() {
            Tokenization result;
            for (;;) {
                try {
                    Token token = next();
                    result.tokens.push_back(token);
                    if (token.kind == Kind::Eof) return result;
                } catch (const Error& error) {
                    result.error.emplace(error.file, error.span, error.what(), error.graph_context);
                    Pos at = error.span.start;
                    result.tokens.push_back({Kind::Eof, names.intern(""), {at, at}});
                    return result;
                }
            }
        }

        std::vector<Token> tokens() {
            Tokenization result = tokenize_recovering();
            if (result.error) throw *result.error;
            return std::move(result.tokens);
        }
    };

    struct Field {
        NameId name = NoName;
        std::vector<NameId> types;
        Span span{};
        NameId union_name = NoName;
    };
    struct Relation {
        NameId left = NoName, tag = NoName, right = NoName;
        Span span{};
    };
    struct Reduce {
        NameId ret = NoName, function = NoName;
        std::vector<NameId> args;
        Span span{};
        bool all = false;
        std::vector<Relation> relations;
        std::vector<NameId> function_candidates;
        std::vector<NameId> function_alternatives;
    };
    using Statement = std::variant<Field, Relation>;
    using WhereItem = std::variant<Relation, Reduce>;

    struct Type {
        NameId universe = NoName, name = NoName, full = NoName;
        std::vector<Statement> inputs;
        std::vector<Statement> outputs;
        std::vector<WhereItem> where;
        Span span{};
        bool has_return = false, has_where = false, return_all = false, return_close = false;
        bool union_template = false, synthetic_variant = false;
        uint64_t template_uid = 0;
        std::vector<NameId> uses;
        uint64_t uid = 0;
        std::string source_file;
        bool function() const { return has_return;
        }
    };
    struct Universe {
        NameId name = NoName;
        std::vector<Type> types;
    };
    struct RunType {
        NameId universe = NoName, name = NoName;
        Span span{};
        std::vector<NameId> uses;
        uint64_t uid = 0;
        std::string source_file;
    };
    struct Program {
        std::vector<Universe> universes;
        std::vector<RunType> runs;
        std::vector<std::pair<NameId, Span>> imports;
    };

    static std::string full_name(Interner& names, NameId universe, NameId name) {
        return std::string(names.str(universe)) + "::" + std::string(names.str(name));
    }

    struct Parser {
        enum class Mode : uint8_t { Input, Output, Where };
        std::string file;
        Interner& names;
        uint64_t& next_uid;
        std::vector<Token> tokens_;
        size_t i = 0;
        Program program;
        size_t universe_index = 0;
        std::optional<Type> type;
        Mode mode = Mode::Input;
        std::vector<NameId> uses;
        uint32_t temp = 0;
        std::optional<Mode> segment_group;
        std::function<void(Program&,NameId,Span)> importer;
        std::unordered_map<NameId, size_t> pending_type_uses;
        std::unordered_map<NameId, Span> first_type_use;

        bool type_declared_here(NameId name) const {
            if (name == NoName || is_literal(names.str(name)) || names.str(name).find("::") != std::string_view::npos) return true;
            NameId universe = program.universes[universe_index].name;
            for (const auto& u : program.universes) {
                if (u.name != universe) continue;
                for (const auto& t : u.types) if (t.name == name) return true;
            }
            return false;
        }

        void note_type_use(NameId name, Span span) {
            if (type_declared_here(name)) return;
            ++pending_type_uses[name];
            first_type_use.try_emplace(name, span);
        }

        Parser(std::string f, std::vector<Token> tokens, Interner& n, uint64_t& uid,
        std::function<void(Program&,NameId,Span)> imp = {})
        : file(std::move(f)), names(n), next_uid(uid), tokens_(std::move(tokens)), importer(std::move(imp)) {
            program.universes.push_back({names.intern("Global"), {}});
        }
        Token& peek(size_t n = 0) {
            return tokens_[std::min(i + n, tokens_.size() - 1)];
        }
        Token take() {
            Token t = peek();
            if (t.kind != Kind::Eof) ++i;
            return t;
        }
        Token expect(std::initializer_list<Kind> kinds) {
            for (Kind k : kinds) if (peek().kind == k) return take();
            std::vector<std::string> expected;
            for (Kind k : kinds) expected.push_back(kind_name(k));
            std::string got = std::string(names.str(peek().text));
            if (got.empty()) got = "<end of file>";
            throw Error(file, peek().span, "syntax error: expected " + join(expected, " or ") + ", but found '" + got + "' (" + kind_name(peek().kind) + "); check the surrounding variable, relation, type annotation, or delimiter");
        }
        void separators() {
            while (peek().kind == Kind::Eol || peek().kind == Kind::Comma) take();
        }
        void inner_eols() {
            while (peek().kind == Kind::Eol) take();
        }
        std::string mode_name(Mode m) const { return m == Mode::Input ? "input" : m == Mode::Output ? "output" : "where";
        }
        Type& current() {
            if (!type) throw Error(file, peek().span, "internal parser state: no current declaration");
            return *type;
        }

        void finish() {
            if (!type) return;
            if (type->has_return && type->return_all && !type->has_where)
            throw Error(file, type->span, "return marker 'all' requires a where block so the return graph can be discovered after evaluating where");
            program.universes[universe_index].types.push_back(std::move(*type));
            type.reset();
            mode = Mode::Input;
        }
        void open_universe() {
            finish();
            take();
            Token n = expect({Kind::Name});
            auto it = std::find_if(program.universes.begin(), program.universes.end(), [&](const Universe& u){
                return u.name == n.text; });
            if (it == program.universes.end()) {
                program.universes.push_back({n.text, {}});
                universe_index = program.universes.size() - 1;
            }
            else universe_index = static_cast<size_t>(std::distance(program.universes.begin(), it));
            uses.clear();
        }
        void open_uses() {
            finish();
            take();
            Token n = expect({Kind::Name});
            if (std::find(uses.begin(), uses.end(), n.text) == uses.end()) uses.push_back(n.text);
        }
        void group_segment(Mode m) {
            if (peek().kind == Kind::LPar) {
                take();
                segment_group = m;
                inner_eols();
        } }
        void open_type() {
            finish();
            Token token = take();
            Token n = expect({Kind::Name, Kind::String});
            bool already_declared = type_declared_here(n.text);
            if (!already_declared) {
                auto used = pending_type_uses.find(n.text);
                if (used != pending_type_uses.end() && used->second != 0) {
                    const Span first = first_type_use.at(n.text);
                    throw Error(file, n.span, "type '" + std::string(names.str(n.text)) + "' is declared after it was already used "
                        + std::to_string(used->second) + (used->second == 1 ? " time" : " times")
                        + " in this universe; first use was at line " + std::to_string(first.start.line));
                }
            }
            Type t{};
            t.universe = program.universes[universe_index].name;
            t.name = n.text;
            t.span = {token.span.start, n.span.end};
            t.uses = uses;
            t.uid = next_uid++;
            t.source_file = file;
            t.full = names.intern(full_name(names, t.universe, t.name));
            type = std::move(t);
            mode = Mode::Input;
            if (peek().kind == Kind::Colon) {
                take();
                Token base = expect({Kind::Name, Kind::String});
                note_type_use(base.text, base.span);
                current().inputs.push_back(Field{names.intern("$"), {base.text}, {n.span.start, base.span.end}});
            }
            group_segment(Mode::Input);
        }
        void open_return() {
            Token token = take();
            if (!type) throw Error(file, token.span, "'return' is not inside a type/function declaration; add it after a 'def ...' declaration");
            if (type->has_return) throw Error(file, token.span, "duplicate return clause in '" + std::string(names.str(type->full)) + "'; a declaration may have only one 'return' or 'return all' clause");
            if (type->has_where) throw Error(file, token.span, "return clause appears after 'where' in '" + std::string(names.str(type->full)) + "'; put 'return'/'return all' before the where block");
            type->has_return = true;
            mode = Mode::Output;
            group_segment(Mode::Output);
        }
        void open_where() {
            Token token = take();
            if (!type) throw Error(file, token.span, "runaway 'where'");
            if (type->has_where) throw Error(file, token.span, "duplicate 'where'");
            type->has_where = true;
            mode = Mode::Where;
            group_segment(Mode::Where);
        }
        NameId temporary() {
            return names.intern("__tmp" + std::to_string(temp++));
        }

        std::unordered_map<NameId, Field*> declared_where_targets() {
            std::unordered_map<NameId, Field*> out;
            if (!type) return out;
            for (auto& s : type->inputs) if (auto* f = std::get_if<Field>(&s)) out.emplace(f->name, f);
            for (auto& s : type->outputs) if (auto* f = std::get_if<Field>(&s)) out.emplace(f->name, f);
            return out;
        }
        NameId root_of(NameId n) {
            std::string_view s = names.str(n);
            size_t p = s.find('.');
            return p == std::string_view::npos ? n : names.intern(s.substr(0, p));
        }
        void require_where_target(NameId ret, Span span, NameId function) {
            if (!type || type->return_all) return;
            NameId root = root_of(ret);
            auto declared = declared_where_targets();
            if (declared.count(root)) return;
            std::vector<std::string> outs, ins;
            for (auto& s : type->outputs) if (auto* f = std::get_if<Field>(&s)) outs.emplace_back(names.str(f->name));
            for (auto& s : type->inputs) if (auto* f = std::get_if<Field>(&s)) ins.emplace_back(names.str(f->name));
            std::string r(names.str(ret));
            std::string detail = r.rfind("__tmp", 0) == 0
            ? "temporary result '" + r + "' created while evaluating '" + std::string(names.str(function)) + "' has no declared destination"
            : "assignment target '" + r + "' for call '" + std::string(names.str(function)) + "' does not exist in the declared input/return graph";
            throw Error(file, span, detail + " in '" + std::string(names.str(type->full)) + "'. This declaration uses an explicit return policy, so a where block may only assign to variables already declared by the input or return shape. Declared inputs: " + (ins.empty()?"<none>":join(ins,", ")) + ". Declared return variables: " + (outs.empty()?"<none>":join(outs,", ")) + ". Declare '" + std::string(names.str(root)) + "' in the return shape, use 'return close' to return the complete input graph, or use 'return all' if the where block is intended to introduce new named or temporary variables.");
        }

        void call(NameId ret, Pos start, std::optional<Token> fn_opt = std::nullopt) {
            Token fn = fn_opt ? *fn_opt : expect({Kind::Name});
            if (peek().kind != Kind::LPar) throw Error(file, peek().span, "function call '" + std::string(names.str(fn.text)) + "' requires parentheses");
            take();
            inner_eols();
            std::vector<NameId> args;
            std::vector<Relation> relations;
            while (peek().kind != Kind::RPar) {
                if (peek().kind == Kind::Eof) throw Error(file, peek().span, "expected ')' to close call to '" + std::string(names.str(fn.text)) + "'");
                auto argument_node = [&]() -> std::pair<NameId, Span> {
                    if (peek().kind == Kind::Name && peek(1).kind == Kind::LPar) {
                        Pos nested_start = peek().span.start;
                        Token nested = take();
                        NameId tmp = temporary();
                        call(tmp, nested_start, nested);
                        return {tmp, {nested_start, tokens_[i - 1].span.end}};
                    }
                    Token node = expect({Kind::Name, Kind::String});
                    Span span = node.span;
                    if (peek().kind == Kind::Colon) {
                        take();
                        Token annotation = expect({Kind::Name, Kind::String});
                        span.end = annotation.span.end;
                    }
                    return {node.text, span};
                };

                auto [left, left_span] = argument_node();
                if (peek().kind == Kind::Rel || peek().kind == Kind::Name) {
                    Token op = take();
                    if (names.str(op.text) == "!" && peek().kind == Kind::Name) {
                        Token tail = take();
                        op.text = names.intern("!" + std::string(names.str(tail.text)));
                        op.span.end = tail.span.end;
                    }
                    auto [right, right_span] = argument_node();
                    args.push_back(left);
                    args.push_back(right);
                    relations.push_back({left, op.text, right, {left_span.start, right_span.end}});
                } else {
                    args.push_back(left);
                }
                inner_eols();
                if (peek().kind == Kind::Comma) {
                    take();
                    inner_eols();
                    if (peek().kind == Kind::RPar) break;
                    continue;
                }
                if (peek().kind != Kind::RPar) throw Error(file, peek().span, "expected ',' or ')' in function call");
            }
            Token rp = expect({Kind::RPar});
            if (args.empty()) throw Error(file, fn.span, "function call '" + std::string(names.str(fn.text)) + "' requires at least one node argument inside parentheses");
            Span call_span{start, rp.span.end};
            require_where_target(ret, call_span, fn.text);
            current().where.push_back(Reduce{ret, fn.text, std::move(args), call_span, false, std::move(relations), {}, {fn.text}});
        }
        void reduce_statement() {
            Token token = take();
            if (peek().kind == Kind::All) {
                take();
                Token fn = expect({Kind::Name});
                std::vector<NameId> alternatives{fn.text};
                Span span{token.span.start, fn.span.end};
                while (peek().kind == Kind::Pipe) {
                    take();
                    Token alternative = expect({Kind::Name});
                    alternatives.push_back(alternative.text);
                    span.end = alternative.span.end;
                }
                Reduce reduction{temporary(), fn.text, {}, span, true, {}, {}, std::move(alternatives)};
                current().where.push_back(std::move(reduction));
                return;
            }
            Token fn = expect({Kind::Name});
            call(temporary(), token.span.start, fn);
        }
        void statement() {
            if (!type) throw Error(file, peek().span, "statement is outside any declaration; start a declaration with def before adding variables or relations");
            if (mode == Mode::Output && peek().kind == Kind::All) {
                take();
                type->return_all = true;
                return;
            }
            if (mode == Mode::Output && peek().kind == Kind::Close) {
                take();
                type->return_close = true;
                return;
            }
            if (mode == Mode::Where && peek().kind == Kind::Reduce) {
                reduce_statement();
                return;
            }
            if (peek().kind == Kind::LPar) {
                take();
                inner_eols();
                statement();
                inner_eols();
                expect({Kind::RPar});
                return;
            }
            Token left = expect({Kind::Name});
            if (peek().kind == Kind::Colon) {
                if (mode == Mode::Where) throw Error(file, left.span, "field inside 'where'");
                take();
                Token first = expect({Kind::Name, Kind::String});
                std::vector<NameId> types{first.text};
                note_type_use(first.text, first.span);
                while (peek().kind == Kind::Pipe) {
                    take();
                    Token alternative = expect({Kind::Name, Kind::String});
                    note_type_use(alternative.text, alternative.span);
                    types.push_back(alternative.text);
                }
                auto& target = mode == Mode::Input ? type->inputs : type->outputs;
                for (auto& s : target) if (auto* f = std::get_if<Field>(&s); f && f->name == left.text) throw Error(file, left.span, "duplicate field '" + std::string(names.str(left.text)) + "'");
                target.push_back(Field{left.text, std::move(types), {left.span.start, tokens_[i-1].span.end}});
                return;
            }
            Token op = expect({Kind::Rel, Kind::Name});
            if (names.str(op.text) == "!" && peek().kind == Kind::Name) {
                Token tail = take();
                op.text = names.intern("!" + std::string(names.str(tail.text)));
                op.span.end = tail.span.end;
            }
            if (mode == Mode::Where && names.str(op.text) == "=") {
                if (peek().kind == Kind::Reduce) {
                    take();
                    call(left.text, left.span.start);
                    return;
                }
                if (peek().kind == Kind::Name && peek(1).kind == Kind::LPar) {
                    Token fn = take();
                    call(left.text, left.span.start, fn);
                    return;
                }
                if (peek().kind == Kind::Name && (peek(1).kind == Kind::Name || peek(1).kind == Kind::String))
                throw Error(file, peek(1).span, "function call '" + std::string(names.str(peek().text)) + "' requires parentheses");
            }
            Token right = expect({Kind::Name, Kind::String});
            Relation r{left.text, op.text, right.text, {left.span.start, right.span.end}};
            if (mode == Mode::Where) type->where.push_back(r);
            else if (mode == Mode::Input) type->inputs.push_back(r);
            else type->outputs.push_back(r);
        }

        Program parse() {
            while (peek().kind != Kind::Eof) {
                separators();
                if (peek().kind == Kind::Eof) break;
                Kind k = peek().kind;
                if (k == Kind::RPar && segment_group && *segment_group == mode) {
                    take();
                    segment_group.reset();
                    continue;
                }
                if (segment_group && *segment_group == mode && (k == Kind::Universe || k == Kind::Uses || k == Kind::Def || k == Kind::Return || k == Kind::Where || k == Kind::Run || k == Kind::Import))
                throw Error(file, peek().span, "expected ')' to close " + mode_name(mode) + " segment");
                switch (k) {
                    case Kind::Universe: open_universe();
                    break;
                    case Kind::Uses: open_uses();
                    break;
                    case Kind::Def: open_type();
                    break;
                    case Kind::Return: open_return();
                    break;
                    case Kind::Where: open_where();
                    break;
                    case Kind::Reduce: if (mode != Mode::Where) throw Error(file, peek().span, "runaway 'reduce'");
                    else reduce_statement();
                    break;
                    case Kind::Do: throw Error(file, peek().span, "'do' is not implemented");
                    case Kind::Import: {
                        finish();
                        Token token = take();
                        Token n = expect({Kind::Name, Kind::String});
                        Span span{token.span.start, n.span.end};
                        program.imports.push_back({n.text, span});
                        if (importer) importer(program, n.text, span);
                        break;
                    }
                    case Kind::Run: { finish();
                        Token token = take();
                        Token n = expect({Kind::Name});
                        RunType run{};
                        run.universe = program.universes[universe_index].name;
                        run.name = n.text;
                        run.span = {token.span.start, n.span.end};
                        run.uses = uses;
                        run.uid = next_uid++;
                        run.source_file = file;
                        program.runs.push_back(std::move(run));
                        break;
                    }
                    default: statement();
                    break;
                }
            }
            if (segment_group) throw Error(file, peek().span, "expected ')' to close " + mode_name(*segment_group) + " segment");
            finish();
            return std::move(program);
        }
    };

    static std::string read_file(const std::filesystem::path& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("cannot open " + path.string());
        return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }

    static void merge_program(Program& dst, const Program& src) {
        std::unordered_set<uint64_t> have_types, have_runs;
        for (auto& u : dst.universes) for (auto& t : u.types) have_types.insert(t.uid);
        for (auto& r : dst.runs) have_runs.insert(r.uid);
        for (const auto& su : src.universes) {
            if (su.types.empty()) continue;
            auto it = std::find_if(dst.universes.begin(), dst.universes.end(), [&](const Universe& u){
                return u.name == su.name; });
            if (it == dst.universes.end()) {
                dst.universes.push_back({su.name,{}});
                it = std::prev(dst.universes.end());
            }
            for (const auto& t : su.types) if (!have_types.count(t.uid)) {
                it->types.push_back(t);
                have_types.insert(t.uid);
            }
        }
        for (const auto& r : src.runs) if (!have_runs.count(r.uid)) {
            dst.runs.push_back(r);
            have_runs.insert(r.uid);
        }
    }

    struct Resolver {
        std::string file;
        Program& program;
        Interner& names;
        uint64_t& next_uid;

        std::vector<NameId> visible_qualified(NameId name, NameId universe, const std::vector<NameId>& uses,
        const std::unordered_map<NameId, std::vector<Type*>>& types, bool functions_only) {
            std::vector<NameId> out;
            std::vector<NameId> search{universe};
            search.insert(search.end(), uses.begin(), uses.end());
            std::set<NameId> seen_universes;
            for (NameId un : search) {
                if (!seen_universes.insert(un).second) continue;
                NameId q = names.intern(std::string(names.str(un)) + "::" + std::string(names.str(name)));
                auto it = types.find(q);
                if (it == types.end()) continue;
                if (functions_only && !std::any_of(it->second.begin(), it->second.end(), [](Type* t){
                    return t->function();
                })) continue;
                out.push_back(q);
            }
            return out;
        }

        NameId resolve_type(NameId name, NameId universe, const std::vector<NameId>& uses, Span span, std::string what,
        const std::unordered_map<NameId, std::vector<Type*>>& types, std::string_view origin_file = {}) {
            std::string_view s = names.str(name);
            std::string origin = origin_file.empty() ? file : std::string(origin_file);
            if (s.find("::") != std::string_view::npos) return name;
            if (is_literal(s)) return names.intern("Impl::" + std::string(s));
            auto candidates = visible_qualified(name, universe, uses, types, false);
            if (candidates.size() == 1) return candidates.front();
            if (candidates.size() > 1) {
                std::vector<std::string> choices;
                for (NameId q : candidates) choices.emplace_back(names.str(q));
                throw Error(origin, span,
                    "ambiguous type '" + std::string(s) + "' while resolving " + what +
                    ": it is visible from multiple universes: " + join(choices, ", ") +
                    ". 'uses' does not choose between type names; qualify the type explicitly with '::' (for example '" +
                    choices.front() + "')");
            }
            std::vector<std::string> order;
            order.emplace_back(names.str(universe));
            for (NameId un : uses) order.emplace_back(names.str(un));
            throw Error(origin, span, "unknown type '" + std::string(s) + "' while resolving " + what +
                ". Visible universes were " + join(order, ", ") +
                ". Qualify the name explicitly, add a 'uses <Universe>' directive, or import the declaration");
        }

        std::vector<NameId> resolve_functions(NameId name, NameId universe, const std::vector<NameId>& uses, Span span, std::string what,
        const std::unordered_map<NameId, std::vector<Type*>>& types, std::string_view origin_file = {}) {
            std::string_view s = names.str(name);
            std::string origin = origin_file.empty() ? file : std::string(origin_file);
            if (s.find("::") != std::string_view::npos) {
                auto it = types.find(name);
                if (it != types.end() && std::any_of(it->second.begin(), it->second.end(), [](Type* t){ return t->function(); }))
                    return {name};
                throw Error(origin, span, "unknown function '" + std::string(s) + "' while resolving " + what);
            }
            auto candidates = visible_qualified(name, universe, uses, types, true);
            if (!candidates.empty()) return candidates;
            std::vector<std::string> order;
            order.emplace_back(names.str(universe));
            for (NameId un : uses) order.emplace_back(names.str(un));
            throw Error(origin, span, "unknown function '" + std::string(s) + "' while resolving " + what +
                ". Function lookup tried every visible universe in this order: " + join(order, " -> ") +
                ". Add a 'uses <Universe>' directive, import the declaration, or qualify the function explicitly with '::'");
        }

        void resolve() {
            NameId impl = names.intern("Impl");
            auto uit = std::find_if(program.universes.begin(), program.universes.end(), [&](const Universe& u){
                return u.name == impl; });
            if (uit == program.universes.end()) {
                program.universes.push_back({impl,{}});
                uit = std::prev(program.universes.end());
            }
            std::set<NameId> literals;
            auto collect_rel = [&](const Relation& r) {
                if (is_literal(names.str(r.left)) && names.str(r.left).find("::") == std::string_view::npos) literals.insert(r.left);
                if (is_literal(names.str(r.right)) && names.str(r.right).find("::") == std::string_view::npos) literals.insert(r.right);
            };
            for (auto& u : program.universes) for (auto& t : u.types) {
                for (auto* vec : {&t.inputs, &t.outputs}) {
                    for (auto& s : *vec) {
                        if (auto* f = std::get_if<Field>(&s)) {
                            for (NameId x : f->types) {
                                if (is_literal(names.str(x)) && names.str(x).find("::") == std::string_view::npos) literals.insert(x);
                            }
                        } else if (auto* r = std::get_if<Relation>(&s)) {
                            collect_rel(*r);
                        }
                    }
                }
                for (auto& w : t.where) {
                    if (auto* r = std::get_if<Relation>(&w)) collect_rel(*r);
                    else if (auto* red = std::get_if<Reduce>(&w)) {
                        for (NameId a : red->args) if (is_literal(names.str(a)) && names.str(a).find("::") == std::string_view::npos) literals.insert(a);
                        for (auto& rr : red->relations) collect_rel(rr);
                    }
                }
            }
            for (NameId lit : literals) {
                bool exists = std::any_of(uit->types.begin(), uit->types.end(), [&](const Type& t){
                    return t.name == lit; });
                if (!exists) {
                    Type t{};
                    t.universe=impl;
                    t.name=lit;
                    t.full=names.intern("Impl::"+std::string(names.str(lit)));
                    t.uid=next_uid++;
                    t.source_file = "<builtin>";
                    uit->types.push_back(std::move(t));
                }
            }

            std::unordered_map<NameId, std::vector<Type*>> types;
            for (auto& u : program.universes) for (auto& t : u.types) {
                if (t.full == NoName) t.full = names.intern(full_name(names, t.universe, t.name));
                types[t.full].push_back(&t);
            }
            for (auto& r : program.runs) {
                std::string origin = r.source_file.empty() ? file : r.source_file;
                r.name = resolve_type(r.name, r.universe, r.uses, r.span, "run target", types, origin);
                auto it = types.find(r.name);
                if (it == types.end()) throw Error(origin, r.span, "cannot run '" + std::string(names.str(r.name)) + "': no declaration with that qualified name exists");
                bool runnable = std::any_of(it->second.begin(), it->second.end(), [](Type* t){
                    return !t->function() || t->return_all; });
                if (!runnable) throw Error(origin, r.span, "cannot run '" + std::string(names.str(r.name)) + "': every declaration with this name has an explicit return shape. 'run' needs either a data-type graph with no return clause or a 'return all' declaration");
            }
            for (auto& u : program.universes) for (auto& t : u.types) {
                std::string origin = t.source_file.empty() ? file : t.source_file;
                std::unordered_map<NameId, std::vector<NameId>> union_cache;
                for (auto* vec : {&t.inputs, &t.outputs}) for (auto& s : *vec) if (auto* f = std::get_if<Field>(&s)) {
                    if (f->types.size() > 1) {
                        std::vector<std::string> raw_parts;
                        for (NameId n : f->types) raw_parts.emplace_back(names.str(n));
                        f->union_name = names.intern(join(raw_parts, "|"));
                        auto cached = union_cache.find(f->union_name);
                        if (cached != union_cache.end()) {
                            f->types = cached->second;
                        } else {
                            for (NameId& n : f->types) n = resolve_type(n, t.universe, t.uses, f->span,
                                "type of variable '" + std::string(names.str(f->name)) + "'", types, origin);
                            union_cache.emplace(f->union_name, f->types);
                        }
                    } else {
                        for (NameId& n : f->types) n = resolve_type(n, t.universe, t.uses, f->span,
                            "type of variable '" + std::string(names.str(f->name)) + "'", types, origin);
                    }
                    for (NameId n : f->types) if (!types.count(n)) throw Error(origin, f->span,
                        "unknown type '" + std::string(names.str(n)) + "' used by variable '" + std::string(names.str(f->name)) + "'");
                }
                for (auto& w : t.where) if (auto* red = std::get_if<Reduce>(&w)) {
                    std::vector<NameId> selectors = red->function_alternatives;
                    if (selectors.empty()) selectors.push_back(red->function);
                    std::vector<NameId> resolved;
                    std::set<NameId> seen_functions;
                    for (NameId selector : selectors) {
                        auto families = resolve_functions(selector, t.universe, t.uses, red->span,
                            "call producing '" + std::string(names.str(red->ret)) + "'", types, origin);
                        for (NameId family : families) if (seen_functions.insert(family).second) resolved.push_back(family);
                    }
                    red->function_candidates = std::move(resolved);
                    red->function = red->function_candidates.front();
                }
            }

            auto materialize_close = [](Type& t) {
                if (!t.return_close) return;
                std::vector<Statement> explicit_outputs = std::move(t.outputs);
                t.outputs = t.inputs;
                t.outputs.insert(t.outputs.end(),
                    std::make_move_iterator(explicit_outputs.begin()),
                    std::make_move_iterator(explicit_outputs.end()));
                t.return_close = false;
            };

            for (auto& u : program.universes) {
                std::vector<Type> expanded;
                for (Type& original : u.types) {
                    struct UnionLocation {
                        bool output = false;
                        size_t index = 0;
                    };
                    std::vector<UnionLocation> unions;
                    for (size_t i = 0; i < original.inputs.size(); ++i) {
                        if (auto* f = std::get_if<Field>(&original.inputs[i]); f && f->types.size() > 1) unions.push_back({false, i});
                    }
                    for (size_t i = 0; i < original.outputs.size(); ++i) {
                        if (auto* f = std::get_if<Field>(&original.outputs[i]); f && f->types.size() > 1) unions.push_back({true, i});
                    }

                    if (unions.empty()) {
                        materialize_close(original);
                        expanded.push_back(std::move(original));
                        continue;
                    }

                    Type source = original;
                    Type display = original;
                    display.union_template = true;
                    display.synthetic_variant = false;
                    display.template_uid = display.uid;
                    materialize_close(display);
                    expanded.push_back(std::move(display));

                    std::vector<Type> variants{std::move(source)};
                    for (const UnionLocation& location : unions) {
                        std::vector<Type> next;
                        for (Type& candidate : variants) {
                            auto& statements = location.output ? candidate.outputs : candidate.inputs;
                            auto* field = std::get_if<Field>(&statements[location.index]);
                            if (!field) continue;
                            std::vector<NameId> alternatives = field->types;
                            for (NameId alternative : alternatives) {
                                Type concrete = candidate;
                                auto& concrete_statements = location.output ? concrete.outputs : concrete.inputs;
                                auto* concrete_field = std::get_if<Field>(&concrete_statements[location.index]);
                                concrete_field->types = {alternative};
                                next.push_back(std::move(concrete));
                            }
                        }
                        variants = std::move(next);
                    }

                    for (Type& concrete : variants) {
                        concrete.union_template = false;
                        concrete.synthetic_variant = true;
                        concrete.template_uid = original.uid;
                        concrete.uid = next_uid++;
                        materialize_close(concrete);
                        expanded.push_back(std::move(concrete));
                    }
                }
                u.types = std::move(expanded);
            }
        }
    };

    struct Loader {
        Interner& names;
        uint64_t next_uid = 1;
        std::unordered_map<std::string, Program> cache;
        std::unordered_set<std::string> loading;
        std::unordered_map<std::string, std::string> sources;
        std::unordered_map<std::string, std::string> source_overrides;
        std::optional<Program> last_partial_program;

        explicit Loader(Interner& n) : names(n) {}
        Program load(const std::string& file, std::optional<std::string> source = std::nullopt) {
            std::filesystem::path path = std::filesystem::weakly_canonical(std::filesystem::absolute(file));
            std::string key = path.string();
            if (!source && !source_overrides.count(key)) {
                auto it = cache.find(key);
                if (it != cache.end()) return it->second;
            }
            if (loading.count(key)) {
                auto it = cache.find(key);
                return it == cache.end() ? Program{} : it->second;
            }
            loading.insert(key);
            std::string text;
            if (source) {
                text = *source;
            } else if (auto it = source_overrides.find(key); it != source_overrides.end()) {
                text = it->second;
            } else {
                text = read_file(path);
            }
            sources[key] = text;

            auto importer = [&](Program& program, NameId imp, Span span) {
                std::string raw;
                if (is_string_literal(names.str(imp))) raw = unquote_string(names.str(imp));
                else {
                    raw = std::string(names.str(imp));
                    std::replace(raw.begin(), raw.end(), '.', '/');
                    raw += ".gs";
                }
                std::filesystem::path import_path(raw);
                std::filesystem::path child = import_path.is_absolute()
                ? import_path.lexically_normal()
                : (std::filesystem::current_path() / import_path).lexically_normal();
                try {
                    Program imported = load(child.string());
                    Resolver(child.string(), imported, names, next_uid).resolve();
                    merge_program(program, imported);
                } catch (const Error&) {
                    if (last_partial_program) merge_program(program, *last_partial_program);
                    throw;
                } catch (const std::exception& e) {
                    throw Error(key, span, "cannot import '" + std::string(names.str(imp)) + "': " + e.what());
                }
            };

            try {
                Lexer lex(key, text, names);
                Lexer::Tokenization tokenization = lex.tokenize_recovering();
                Parser parser(key, std::move(tokenization.tokens), names, next_uid, importer);
                try {
                    Program parsed = parser.parse();
                    if (tokenization.error) {
                        last_partial_program = parsed;
                        throw *tokenization.error;
                    }
                    loading.erase(key);
                    cache[key] = parsed;
                    return parsed;
                } catch (const Error&) {
                    last_partial_program = parser.program;
                    if (tokenization.error) throw *tokenization.error;
                    throw;
                }
            } catch (...) {
                loading.erase(key);
                throw;
            }
        }
    };

    struct TypeRegistry {
        Interner& names;
        std::unordered_map<NameId, uint8_t> bit_of;
        std::array<NameId,64> name_of{};
        uint8_t count = 0;
        explicit TypeRegistry(Interner& n) : names(n) {
            name_of.fill(NoName);
        }
        TypeBits bit(NameId qualified) {
            auto it = bit_of.find(qualified);
            if (it != bit_of.end()) return TypeBits{1} << it->second;
            if (count >= 64) throw std::runtime_error("more than 64 atomic type tags are required; widen TypeBits to continue");
            uint8_t b = count++;
            bit_of.emplace(qualified,b);
            name_of[b]=qualified;
            return TypeBits{1} << b;
        }
        std::vector<NameId> names_for(TypeBits bits) const {
            std::vector<NameId> out;
            for (uint8_t b=0;b<count;++b) if (bits&(TypeBits{1}<<b)) out.push_back(name_of[b]);
            return out;
        }
        std::string format(TypeBits bits) const {
            std::vector<std::string> out;
            for (NameId n:names_for(bits)) out.emplace_back(names.str(n));
            return out.empty()?"<none>":join(out," | ");
        }
    };

    struct Alias {
        NameId theory=NoName, name=NoName;
        friend bool operator==(const Alias&,const Alias&)=default;
    };
    struct Node {
        NodeId id = NoNode;
        TypeBits types = 0;
        bool alive = true;
        std::vector<Alias> names;
        std::vector<NameId> input_names;

        // Runtime-only ownership tracking. Graph construction leaves these zeroed.
        void* memory = nullptr;
        uint32_t outgoing_edges = 0;
        uint32_t used_outgoing_edges = 0;
    };
    struct Edge {
        NodeId left=NoNode;
        NameId tag=NoName;
        NodeId right=NoNode;
        friend bool operator==(const Edge&,const Edge&)=default;
        friend bool operator<(const Edge&a,const Edge&b){
            return std::tie(a.left,a.tag,a.right)<std::tie(b.left,b.tag,b.right);
        }
    };

    struct Graph {
        Interner* names = nullptr;
        TypeRegistry* types = nullptr;
        std::vector<Node> nodes;
        std::vector<Edge> edges;

        Graph() = default;
        Graph(Interner& n, TypeRegistry& t) : names(&n), types(&t) {}
        NodeId add_node(TypeBits bits, NameId theory=NoName, NameId name=NoName) {
            NodeId id = static_cast<NodeId>(nodes.size());
            Node n{};
            n.id = id;
            n.types = bits;
            if (theory != NoName || name != NoName) n.names.push_back({theory,name});
            nodes.push_back(std::move(n));
            return id;
        }
        bool has(NodeId i) const { return i<nodes.size() && nodes[i].alive;
        }
        std::vector<NodeId> ids() const { std::vector<NodeId> out;
            for(auto& n:nodes) if(n.alive) out.push_back(n.id);
            return out;
        }
        void add_alias(NodeId i, NameId theory, NameId name) {
            if (!has(i)) return;
            Alias a{theory,name};
            auto& xs = nodes[i].names;
            if (std::find(xs.begin(),xs.end(),a)==xs.end()) xs.push_back(a);
        }
        void add_input_name(NodeId i, NameId name) {
            if(has(i)&&std::find(nodes[i].input_names.begin(),nodes[i].input_names.end(),name)==nodes[i].input_names.end()) nodes[i].input_names.push_back(name);
        }
        std::optional<NodeId> lookup(NameId name, std::optional<NameId> theory=std::nullopt) const {
            for (auto& n : nodes) {
                if (!n.alive) continue;
                for (auto& a : n.names) {
                    if (a.name==name && (!theory || a.theory==*theory)) return n.id;
                }
            }
            return std::nullopt;
        }
        void add_edge(Edge e) {
            if(std::find(edges.begin(),edges.end(),e)==edges.end()) edges.push_back(e);
        }
        NodeId merge(NodeId a,NodeId b,Span span={},std::string file={}) {
            if (a==b) return a;
            if (!has(a)||!has(b)) throw std::logic_error("merge dead node");
            Node& x=nodes[a];
            Node& y=nodes[b];
            if (x.types!=y.types) throw Error(std::move(file),span,"cannot merge variables: their type sets differ ("+types->format(x.types)+" vs "+types->format(y.types)+"). Identity merges require exactly compatible types");
            for (auto al:y.names) add_alias(a,al.theory,al.name);
            for (NameId n:y.input_names) add_input_name(a,n);
            for (auto& e:edges) {
                if(e.left==b)e.left=a;
                if(e.right==b)e.right=a;
            }
            std::sort(edges.begin(),edges.end());
            edges.erase(std::unique(edges.begin(),edges.end()),edges.end());
            y.alive=false;
            return a;
        }
        std::pair<Graph,std::unordered_map<NodeId,NodeId>> induced(const std::set<NodeId>& wanted) const {
            Graph g(*names,*types);
            std::unordered_map<NodeId,NodeId> remap;
            for(NodeId old:wanted) if(has(old)){
                NodeId neu=g.add_node(nodes[old].types);
                g.nodes[neu].names=nodes[old].names;
                g.nodes[neu].input_names=nodes[old].input_names;
                remap[old]=neu;
            }
            for (auto& e:edges) {
                if (remap.count(e.left)&&remap.count(e.right)) g.add_edge({remap[e.left],e.tag,remap[e.right]});
            }
            return {std::move(g),std::move(remap)};
        }
        void erase_nodes(const std::set<NodeId>& remove){
            for(NodeId i:remove)if(has(i))nodes[i].alive=false;
            edges.erase(std::remove_if(edges.begin(),edges.end(),[&](const Edge&e){
                return !has(e.left)||!has(e.right);}),edges.end());
        }
        void remove_component(std::set<NodeId> remove,const std::set<NodeId>& protected_ids,const std::string& file,Span span,std::optional<std::set<NodeId>> within_opt=std::nullopt) {
            std::set<NodeId> within;
            if (within_opt) within=*within_opt;
            else {
                auto live=ids();
                within.insert(live.begin(),live.end());
            }
            for(NodeId i:remove)if(protected_ids.count(i))throw Error(file,span,"reduction cleanup would drop an output node");
            for(;;){
                std::set<NodeId> add;
                for(auto&e:edges){
                    if(remove.count(e.left)&&within.count(e.right)&&!remove.count(e.right)&&!protected_ids.count(e.right))add.insert(e.right);
                    if(remove.count(e.right)&&within.count(e.left)&&!remove.count(e.left)&&!protected_ids.count(e.left))add.insert(e.left);
                }if(add.empty())break;
                remove.insert(add.begin(),add.end());
            }
            size_t alive=ids().size();
            size_t doomed=0;
            for(NodeId i:remove)if(has(i))++doomed;
            if(alive==doomed)throw Error(file,span,"reduction cleanup would remove the entire graph; at least one node must remain after removing the unused reduction component");
            erase_nodes(remove);
        }
    };


    static std::vector<NameId> node_aliases(const Graph& g, NodeId i) {
        std::vector<NameId> out;
        if (!g.has(i)) return out;
        for (auto a : g.nodes[i].names) if (a.name != NoName && std::find(out.begin(),out.end(),a.name)==out.end()) out.push_back(a.name);
        std::sort(out.begin(),out.end(),[&](NameId a,NameId b){
            auto sa=g.names->str(a),sb=g.names->str(b);return std::pair{sa.size(),sa}<std::pair{sb.size(),sb};});
        return out;
    }
    static NameId best_name_id(const Graph& g, NodeId i) {
        if (!g.has(i)) return NoName;
        auto xs=g.nodes[i].input_names;
        if(xs.empty()) xs=node_aliases(g,i);
        if(xs.empty()) return NoName;
        std::sort(xs.begin(),xs.end(),[&](NameId a,NameId b){
            auto sa=g.names->str(a),sb=g.names->str(b);return std::pair{sa.size(),sa}<std::pair{sb.size(),sb};});
        return xs.front();
    }
    static std::string best_name(const Graph& g,NodeId i){
        NameId n=best_name_id(g,i);
        return n==NoName?"#"+std::to_string(i):std::string(g.names->str(n));
    }
    static std::string graph_diagnostic(const Graph& g){
        std::vector<std::string> vars,rels;
        for(NodeId i:g.ids())vars.push_back(best_name(g,i)+" : "+g.types->format(g.nodes[i].types));
        for(auto&e:g.edges)if(g.has(e.left)&&g.has(e.right))rels.push_back(best_name(g,e.left)+" "+std::string(g.names->str(e.tag))+" "+best_name(g,e.right));
        return "variables/types: "+(vars.empty()?std::string("<none>"):join(vars,", "))+"\nrelations: "+(rels.empty()?std::string("<none>"):join(rels,", "));
    }

    static std::optional<NameId> call_name_id(const Graph& g,NodeId id){
        if (!g.has(id)) return std::nullopt;
        std::map<std::string, NameId> literals;
        for (const Alias& alias : g.nodes[id].names) {
            std::string_view q = g.names->str(alias.name);
            if (!is_string_literal(q)) continue;
            std::string raw(literal_text(q));
            literals.emplace(raw, alias.name);
        }
        if (literals.size() != 1) return std::nullopt;
        NameId literal = literals.begin()->second;
        std::string_view q = g.names->str(literal);
        size_t p=q.rfind("::");
        std::string u=p==std::string_view::npos?"Impl":std::string(q.substr(0,p));
        std::string name=unquote_string(std::string(literal_text(q)));
        return g.names->intern(u+"::"+name);
    }

    struct Builder {
        using NodeMap=std::unordered_map<NameId,NodeId>;
        std::string file;
        Program& program;
        Interner& names;
        TypeRegistry& registry;
        std::unordered_map<NameId,std::vector<const Type*>> groups;
        std::unordered_map<NameId,const Type*> selected;

        Builder(std::string f,Program&p,Interner&n,TypeRegistry&r,std::unordered_map<NameId,const Type*> sel={})
        :file(std::move(f)),program(p),names(n),registry(r),selected(std::move(sel)){
            for(auto&u:program.universes)for(auto&t:u.types)if(!t.union_template)groups[t.full].push_back(&t);
            if(selected.empty())for(auto&[k,v]:groups)selected[k]=v.back();
        }
        std::vector<const Type*> function_overloads(const Reduce& r) const {
            std::vector<const Type*> out;
            std::vector<NameId> families = r.function_candidates;
            if (families.empty()) families.push_back(r.function);
            std::set<const Type*> seen;
            for (NameId q : families) {
                auto it = groups.find(q);
                if (it == groups.end()) continue;
                for (const Type* fn : it->second) {
                    if (!fn->function() || !seen.insert(fn).second) continue;
                    out.push_back(fn);
                }
            }
            return out;
        }
        std::vector<std::unordered_map<NameId,const Type*>> selection_variants(const Type& root) const {
            std::set<NameId> refs;
            std::set<NameId> visiting;
            std::function<void(NameId)> collect = [&](NameId ref) {
                if (ref == root.full || !visiting.insert(ref).second) return;
                auto it = groups.find(ref);
                if (it == groups.end()) return;
                refs.insert(ref);
                for (const Type* declaration : it->second) {
                    for (const auto* statements : {&declaration->inputs, &declaration->outputs}) {
                        for (const Statement& statement : *statements) {
                            if (const auto* field = std::get_if<Field>(&statement)) {
                                for (NameId child : field->types) collect(child);
                            }
                        }
                    }
                }
            };
            for (const auto* statements : {&root.inputs, &root.outputs}) {
                for (const Statement& statement : *statements) {
                    if (const auto* field = std::get_if<Field>(&statement)) {
                        for (NameId ref : field->types) collect(ref);
                    }
                }
            }

            std::vector<std::unordered_map<NameId,const Type*>> variants{selected};
            for (NameId ref : refs) {
                auto it = groups.find(ref);
                if (it == groups.end() || it->second.empty()) continue;
                std::vector<std::unordered_map<NameId,const Type*>> next;
                for (const auto& base : variants) {
                    for (const Type* declaration : it->second) {
                        auto branch = base;
                        branch[ref] = declaration;
                        next.push_back(std::move(branch));
                    }
                }
                variants = std::move(next);
            }
            for (auto& variant : variants) variant[root.full] = &root;
            return variants;
        }

        NameId qualify(NameId prefix,NameId n){
            if(prefix==NoName||names.str(prefix).empty())return n;
            return names.intern(std::string(names.str(prefix))+"."+std::string(names.str(n)));
        }
        bool in_stack(NameId x,const std::vector<NameId>& stack){
            return std::find(stack.begin(),stack.end(),x)!=stack.end();
        }
        bool atomic(NameId q)const{auto it=selected.find(q);
            return it!=selected.end()&&it->second->inputs.empty()&&!it->second->function();
        }

        NodeMap add_shape(Graph&g,const Type&t,NameId prefix,NameId theory,bool output=false,std::vector<NameId> stack={}){
            if(in_stack(t.full,stack))throw Error(file,t.span,"recursive type "+std::string(names.str(t.full)));
            const auto& ss=output?t.outputs:t.inputs;
            NodeMap out;
            if(!output&&ss.size()==1){
                if(auto*x=std::get_if<Field>(&ss[0]);x&&names.str(x->name)=="$"){
                    NameId target=x->types.at(0);
                    auto it=selected.find(target);
                    if(it==selected.end())throw Error(file,x->span,"alias refers to unknown type '"+std::string(names.str(target))+"'");
                    const Type&chosen=*it->second;
                    if(chosen.inputs.empty()&&!chosen.function()){
                        NameId nm=(prefix==NoName||names.str(prefix).empty())?t.name:prefix;
                        NameId base = is_literal(names.str(target)) ? literal_base_type(names, target) : NoName;
                        NodeId id=g.add_node(registry.bit(base == NoName ? target : base),theory,nm);
                        if (base != NoName) g.add_alias(id, theory, target);
                        out[nm]=id;
                        return out;
                    }
                    stack.push_back(t.full);
                    return add_shape(g,chosen,prefix,theory,false,std::move(stack));
            }}
            for(auto&s:ss){
                auto*x=std::get_if<Field>(&s);
                if(!x)continue;
                TypeBits bits=0;
                std::vector<NameId> structured;
                std::vector<NameId> literal_aliases;
                for(NameId q:x->types){
                    if (is_literal(names.str(q))) {
                        NameId base = literal_base_type(names, q);
                        if (base == NoName) throw Error(file, x->span, "cannot determine base type for literal '" + std::string(names.str(q)) + "'");
                        bits |= registry.bit(base);
                        literal_aliases.push_back(q);
                    } else if(atomic(q))bits|=registry.bit(q);
                    else structured.push_back(q);
                }NameId nm=qualify(prefix,x->name);
                if(bits){
                    NodeId id=g.add_node(bits,theory,nm);
                    for (NameId literal : literal_aliases) g.add_alias(id, theory, literal);
                    out[nm]=id;
                }for(NameId child:structured){
                    auto it=selected.find(child);
                    if(it==selected.end())throw Error(file,x->span,"unknown type '"+std::string(names.str(child))+"'");
                    auto ns=stack;
                    ns.push_back(t.full);
                    NodeMap sub=add_shape(g,*it->second,nm,theory,it->second->function(),std::move(ns));
                    out.insert(sub.begin(),sub.end());
            }}
            for(auto&s:ss){
                auto*r=std::get_if<Relation>(&s);
                if(!r)continue;
                NameId a=qualify(prefix,r->left),b=qualify(prefix,r->right);
                auto ia=out.find(a),ib=out.find(b);
                if(ia==out.end()||ib==out.end())throw Error(file,r->span,"cannot add relation '"+std::string(names.str(r->left))+" "+std::string(names.str(r->tag))+" "+std::string(names.str(r->right))+"': one or both endpoints do not resolve to leaf variables");
                g.add_edge({ia->second,r->tag,ib->second});
            }
            return out;
        }
        std::pair<Graph,NodeMap> function_input(const Type&t){
            Graph g(names,registry);
            auto m=add_shape(g,t,NoName,t.universe,false);
            return {std::move(g),std::move(m)};
        }

        std::set<NodeId> named_members(const Graph&g,NameId name,Span span,std::string role="variable"){
            std::set<NodeId> found;
            std::string n(names.str(name)),pre=n+".";
            const bool exact_only = is_literal(n);
            for(auto&node:g.nodes)if(node.alive)for(auto a:node.names){
                std::string_view s=names.str(a.name);
                if(s==n||(!exact_only&&s.starts_with(pre)))found.insert(node.id);
            }if(found.empty()){
                std::vector<std::string> avail;
                for(auto&node:g.nodes)if(node.alive)for(auto a:node.names)if(a.name!=NoName&&!names.str(a.name).empty())avail.emplace_back(names.str(a.name));
                throw Error(file,span,"call relation "+role+" '"+n+"' does not resolve to an existing graph variable or structured-variable prefix. Available variables: "+(avail.empty()?"<none>":join(avail,", ")),graph_diagnostic(g));
            }return found;
        }
        std::set<NodeId> expand_args(const Graph&g,const std::vector<NameId>&args,Span span){
            std::set<NodeId> out;
            NameId returns=names.intern("returns");
            for(NameId arg:args){
                auto m=named_members(g,arg,span,"argument");
                std::set<NodeId> returned;
                for(auto&e:g.edges)if(e.tag==returns&&m.count(e.left)&&m.count(e.right))returned.insert(e.right);
                if(!returned.empty())m=std::move(returned);
                out.insert(m.begin(),m.end());
            }return out;
        }
        std::set<NameId> edge_sig(const Graph&g,NodeId a,NodeId b,bool negative=false)const{std::set<NameId> out;
            for(auto&e:g.edges)if(e.left==a&&e.right==b){
                std::string_view t=names.str(e.tag);
                if(negative){
                    if(t.starts_with("!"))out.insert(names.intern(t.substr(1)));
                }else if(!t.starts_with("!"))out.insert(e.tag);
            }return out;
        }
        bool compatible_edges(const Graph&a,const Graph&b,NodeId x,NodeId xx,NodeId y,NodeId yy)const{auto expected=edge_sig(a,x,xx,false),actual=edge_sig(b,y,yy,false);
            if(!std::includes(actual.begin(),actual.end(),expected.begin(),expected.end()))return false;
            auto neg=edge_sig(a,x,xx,true);
            for(NameId n:neg)if(actual.count(n))return false;
            return true;
        }

        std::set<std::string> literal_aliases(const Graph& g, NodeId id) const {
            std::set<std::string> out;
            for (const Alias& alias : g.nodes[id].names) {
                if (is_literal(names.str(alias.name))) out.insert(std::string(literal_text(names.str(alias.name))));
            }
            return out;
        }
        bool literal_constraints_match(const Graph& expected, NodeId x, const Graph& actual, NodeId y) const {
            auto need = literal_aliases(expected, x);
            if (need.empty()) return true;
            auto have = literal_aliases(actual, y);
            return std::includes(have.begin(), have.end(), need.begin(), need.end());
        }

        std::optional<std::unordered_map<NodeId,NodeId>> iso_impl(const Graph&a,const Graph&b,const std::unordered_map<NodeId,std::set<NodeId>>*allowed,bool subgraph){
            auto aids=a.ids(),bids=b.ids();
            if(!subgraph&&aids.size()!=bids.size())return std::nullopt;
            std::unordered_map<NodeId,std::vector<NodeId>> cand;
            for(NodeId x:aids){
                for(NodeId y:bids)if(a.nodes[x].types==b.nodes[y].types&&literal_constraints_match(a,x,b,y)&&(!allowed||!allowed->count(x)||allowed->at(x).count(y)))cand[x].push_back(y);
                if(cand[x].empty())return std::nullopt;
            }
            std::sort(aids.begin(),aids.end(),[&](NodeId x,NodeId z){
                auto deg=[&](NodeId q){
                    size_t d=0;for(auto&e:a.edges)if(e.left==q||e.right==q)++d;return d;};auto dx=deg(x), dz=deg(z); if(cand[x].size()!=cand[z].size()) return cand[x].size()<cand[z].size(); return dx>dz;});
            std::unordered_map<NodeId,NodeId> m;
            std::set<NodeId> used;
            std::function<bool(size_t)> dfs=[&](size_t idx){
                if(idx==aids.size())return true;
                NodeId x=aids[idx];
                for(NodeId y:cand[x]){
                    if(used.count(y))continue;
                    bool ok=true;
                    for(auto[xx,yy]:m)if(!compatible_edges(a,b,x,xx,y,yy)||!compatible_edges(a,b,xx,x,yy,y)){
                        ok=false;
                        break;
                    }if(!ok)continue;
                    m[x]=y;
                    used.insert(y);
                    if(dfs(idx+1))return true;
                    used.erase(y);
                    m.erase(x);
                }return false;
            };
            if(!dfs(0))return std::nullopt;
            return m;
        }
        std::optional<std::unordered_map<NodeId,NodeId>> isomorphism(const Graph&a,const Graph&b,const std::unordered_map<NodeId,std::set<NodeId>>*allowed=nullptr){
            return iso_impl(a,b,allowed,false);
        }
        std::optional<std::unordered_map<NodeId,NodeId>> subgraph_isomorphism(const Graph&a,const Graph&b){
            return iso_impl(a,b,nullptr,true);
        }

        std::string node_description(const Graph& g, NodeId id) const {
            std::vector<std::string> aliases;
            for (const Alias& alias : g.nodes[id].names) {
                if (alias.name == NoName) continue;
                aliases.emplace_back(names.str(alias.name));
            }
            std::string out = registry.format(g.nodes[id].types);
            if (!aliases.empty()) out += " [" + join(aliases, ", ") + "]";
            return out;
        }

        std::string edge_description(const Graph& g, const Edge& edge) const {
            return node_description(g, edge.left) + " --" + std::string(names.str(edge.tag)) + "--> " + node_description(g, edge.right);
        }

        std::string mismatch(const Graph& expected, const Graph& actual) {
            std::vector<std::string> lines;
            auto expected_ids = expected.ids();
            auto actual_ids = actual.ids();
            if (expected_ids.size() != actual_ids.size()) {
                lines.push_back("node count: expected " + std::to_string(expected_ids.size()) + ", got " + std::to_string(actual_ids.size()));
            }

            std::unordered_map<NodeId, NodeId> pairing;
            std::set<NodeId> used_actual;
            for (NodeId e : expected_ids) {
                for (NodeId a : actual_ids) {
                    if (used_actual.count(a)) continue;
                    if (expected.nodes[e].types != actual.nodes[a].types) continue;
                    if (!literal_constraints_match(expected, e, actual, a)) continue;
                    pairing[e] = a;
                    used_actual.insert(a);
                    break;
                }
            }

            std::vector<std::string> missing_nodes;
            for (NodeId e : expected_ids) {
                if (!pairing.count(e)) missing_nodes.push_back(node_description(expected, e));
            }
            std::vector<std::string> extra_nodes;
            for (NodeId a : actual_ids) {
                if (!used_actual.count(a)) extra_nodes.push_back(node_description(actual, a));
            }
            if (!missing_nodes.empty()) {
                lines.push_back("missing nodes:");
                for (const auto& item : missing_nodes) lines.push_back("  - " + item);
            }
            if (!extra_nodes.empty()) {
                lines.push_back("extra/unmatched nodes:");
                for (const auto& item : extra_nodes) lines.push_back("  - " + item);
            }

            std::vector<std::string> missing_edges;
            for (const Edge& e : expected.edges) {
                auto l = pairing.find(e.left);
                auto r = pairing.find(e.right);
                if (l == pairing.end() || r == pairing.end()) continue;
                bool found = std::any_of(actual.edges.begin(), actual.edges.end(), [&](const Edge& a) {
                    return a.left == l->second && a.right == r->second && a.tag == e.tag;
                });
                if (!found) missing_edges.push_back(edge_description(expected, e));
            }
            if (!missing_edges.empty()) {
                lines.push_back("missing edges:");
                for (const auto& item : missing_edges) lines.push_back("  - " + item);
            }

            std::vector<std::string> extra_edges;
            for (const Edge& a : actual.edges) {
                bool found = false;
                for (const auto& [e_id, a_id] : pairing) {
                    if (a_id != a.left) continue;
                    for (const auto& [er_id, ar_id] : pairing) {
                        if (ar_id != a.right) continue;
                        found = std::any_of(expected.edges.begin(), expected.edges.end(), [&](const Edge& e) {
                            return e.left == e_id && e.right == er_id && e.tag == a.tag;
                        });
                        if (found) break;
                    }
                    if (found) break;
                }
                if (!found) extra_edges.push_back(edge_description(actual, a));
            }
            if (!extra_edges.empty()) {
                lines.push_back("extra/unmatched edges:");
                for (const auto& item : extra_edges) lines.push_back("  - " + item);
            }

            return lines.empty() ? "node identities/relations cannot be mapped consistently" : join(lines, "\n");
        }

        NodeId bind_literal(Graph& g, const Type& owner, NameId variable, NameId value, Span span) {
            std::string_view raw = literal_text(names.str(value));
            NameId literal = names.intern(std::string(raw));
            NameId qualified = names.intern("Impl::" + std::string(raw));
            NameId base = literal_base_type(names, qualified);
            if (base == NoName) throw Error(file, span, "cannot determine base type for literal '" + std::string(raw) + "'");
            TypeBits bit = registry.bit(base);
            auto existing = g.lookup(variable);
            NodeId id;
            if (existing) {
                id = *existing;
                if ((g.nodes[id].types & bit) == 0) {
                    throw Error(file, span, "literal '" + std::string(raw) + "' has base type " + std::string(names.str(base)) + " but variable '" + std::string(names.str(variable)) + "' has type set " + registry.format(g.nodes[id].types), graph_diagnostic(g));
                }
            } else {
                id = g.add_node(bit, owner.universe, variable);
            }
            g.add_alias(id, owner.universe, literal);
            g.add_alias(id, owner.universe, qualified);
            return id;
        }

        NodeId materialize_literal(Graph&g,const Type&owner,NameId value,Span span){
            if(auto i=g.lookup(value))return*i;
            if(!is_literal(names.str(value)))throw Error(file,span,"internal error: requested non-literal");
            if(!owner.return_all)throw Error(file,span,"literal '"+std::string(names.str(value))+"' inside where needs an implicit temporary; implicit where temporaries require 'return all'");
            NameId q=names.intern("Impl::"+std::string(names.str(value)));
            NameId base=literal_base_type(names,q);
            if(base==NoName)throw Error(file,span,"cannot determine base type for literal '"+std::string(names.str(value))+"'");
            NameId nm=names.intern("__literal"+std::to_string(g.nodes.size()));
            NodeId i=g.add_node(registry.bit(base),owner.universe,nm);
            g.add_alias(i,owner.universe,q);
            g.add_alias(i,owner.universe,value);
            return i;
        }
        void materialize_reduce_literals(Graph&g,const Type&owner,const Reduce&r){
            std::set<NameId> vals;
            for(NameId x:r.args)if(is_literal(names.str(x)))vals.insert(x);
            for(auto&rel:r.relations){
                if(is_literal(names.str(rel.left)))vals.insert(rel.left);
                if(is_literal(names.str(rel.right)))vals.insert(rel.right);
            }for(NameId x:vals)materialize_literal(g,owner,x,r.span);
        }

        struct FunctionBuild{Graph graph;
            NodeMap inputs,outputs;
        };
        FunctionBuild build_function(const Type&t);
        void apply_reduce(Graph&g,const Type&owner,const Reduce&r,std::optional<std::set<NodeId>> selected_override=std::nullopt,const Type* forced_fn=nullptr,const std::unordered_map<NodeId,NodeId>* forced_match=nullptr);
        std::string graph_shape(const Graph&g){
            std::vector<std::string> ns,es;
            for(NodeId i:g.ids())ns.push_back(std::to_string(g.nodes[i].types));
            std::sort(ns.begin(),ns.end());
            for(auto&e:g.edges)if(g.has(e.left)&&g.has(e.right))es.push_back(std::to_string(g.nodes[e.left].types)+":"+std::string(names.str(e.tag))+":"+std::to_string(g.nodes[e.right].types));
            std::sort(es.begin(),es.end());
            return join(ns,",")+"|"+join(es,",");
        }
        void apply_all(Graph&g,const Type&owner,const Reduce&r){
            std::set<std::string> seen;
            for(size_t round=0;;++round){
                std::string shape=graph_shape(g);
                if(seen.count(shape))return;
                seen.insert(shape);
                struct Choice{size_t rarity;
                    const Type*fn;
                    std::unordered_map<NodeId,NodeId>match;
                    std::unordered_map<NameId,const Type*> sel;
                };
                std::vector<Choice> choices;
                auto overloads=function_overloads(r);
                if(overloads.empty())return;
                for(const Type*fn:overloads){
                    for(auto sel:selection_variants(*fn)){
                        Builder cb(file,program,names,registry,sel);
                        auto[expected,emap]=cb.function_input(*fn);
                        (void)emap;
                        auto match=cb.subgraph_isomorphism(expected,g);
                        if(match){
                            size_t rarity=std::numeric_limits<size_t>::max();
                            for(NodeId x:expected.ids()){
                                size_t c=0;
                                for(NodeId y:g.ids())if(g.nodes[y].types==expected.nodes[x].types)++c;
                                rarity=std::min(rarity,c);
                            }
                            choices.push_back({rarity,fn,*match,std::move(sel)});
                        }
                    }
                }if(choices.empty())return;
                std::sort(choices.begin(),choices.end(),[](auto&a,auto&b){
                    return a.rarity<b.rarity;});
                Reduce tmp{names.intern(std::string(names.str(r.ret))+std::to_string(round)),r.function,{},r.span,false,{},r.function_candidates,r.function_alternatives};
                std::set<NodeId> ids;
                for(auto[k,v]:choices[0].match){
                    (void)k;
                    ids.insert(v);
                }
                Builder chosen(file,program,names,registry,choices[0].sel);
                chosen.apply_reduce(g,owner,tmp,ids,choices[0].fn,&choices[0].match);
        }}
    };


    Builder::FunctionBuild Builder::build_function(const Type&t){
        auto root_name=[&](NameId n){
            std::string_view s=names.str(n);
            size_t p=s.find('.');
            return p==std::string_view::npos?n:names.intern(s.substr(0,p));
        };
        if(t.return_all){
            Graph g(names,registry);
            NodeMap ins=add_shape(g,t,NoName,t.universe,false);
            for(auto[name,id]:ins)g.add_input_name(id,name);
            std::set<NameId> visible;
            for(auto[name,id]:ins){
                (void)id;
                visible.insert(root_name(name));
            }
            for(auto&w:t.where){
                if(auto*rel=std::get_if<Relation>(&w)){
                    if(names.str(rel->tag)!="=")throw Error(file,rel->span,"invalid where relation in '"+std::string(names.str(t.full))+"': where-level relations merge variable identities and must use '='",graph_diagnostic(g));
                    bool left_literal = is_literal(names.str(rel->left));
                    bool right_literal = is_literal(names.str(rel->right));
                    if (left_literal != right_literal) {
                        NameId variable = left_literal ? rel->right : rel->left;
                        NameId literal = left_literal ? rel->left : rel->right;
                        bind_literal(g, t, variable, literal, rel->span);
                    } else {
                        auto a=g.lookup(rel->left),b=g.lookup(rel->right);
                        if(!a||!b)throw Error(file,rel->span,"cannot merge where variables '"+std::string(names.str(rel->left))+" = "+std::string(names.str(rel->right))+"': unknown variable",graph_diagnostic(g));
                        g.merge(*a,*b,rel->span,file);
                    }
                }else{
                    const auto& source_reduce=std::get<Reduce>(w);
                    Reduce r=source_reduce;
                    std::vector<NameId> families=r.function_candidates;
                    if(families.empty())families.push_back(r.function);
                    bool had_recursive=std::find(families.begin(),families.end(),t.full)!=families.end();
                    if(had_recursive){
                        families.erase(std::remove(families.begin(),families.end(),t.full),families.end());
                        if(families.empty())throw Error(file,r.span,"cannot infer 'return all' for recursive call inside '"+std::string(names.str(t.full))+"'; declare an explicit return shape",graph_diagnostic(g));
                        r.function_candidates=std::move(families);
                        r.function=r.function_candidates.front();
                    }
                    std::string_view rr=names.str(r.ret);
                    if(!r.all&&!rr.starts_with("__tmp"))visible.insert(root_name(r.ret));
                    if(r.all)apply_all(g,t,r);
                    else apply_reduce(g,t,r);
                }
            }
            NodeMap refreshed;
            for(auto[name,id]:ins){
                (void)id;
                auto gid=g.lookup(name,t.universe);
                if(gid)refreshed[name]=*gid;
            }ins=std::move(refreshed);
            std::vector<Field> explicit_fields;
            for(auto&s:t.outputs)if(auto*f=std::get_if<Field>(&s))explicit_fields.push_back(*f);
            std::set<NameId> explicit_roots;
            for(auto&f:explicit_fields)explicit_roots.insert(root_name(f.name));
            if(!explicit_fields.empty()){
                Type shape{};
                shape.universe=t.universe;
                shape.name=t.name;
                shape.full=t.full;
                shape.has_return=true;
                shape.span=t.span;
                shape.uses=t.uses;
                for(auto&f:explicit_fields)shape.outputs.push_back(f);
                NodeMap declared=add_shape(g,shape,NoName,t.universe,true);
                for(auto[name,newid]:declared){
                    std::vector<NodeId> others;
                    for(NodeId i:g.ids())if(i!=newid)for(auto a:g.nodes[i].names)if(a.theory==t.universe&&a.name==name){
                        others.push_back(i);
                        break;
                    }for(NodeId old:others){
                        if(!g.has(newid)||!g.has(old))continue;
                        if(g.nodes[newid].types!=g.nodes[old].types)throw Error(file,t.span,"explicit return field '"+std::string(names.str(name))+"' conflicts with variable discovered by where",graph_diagnostic(g));
                        newid=g.merge(old,newid,t.span,file);
                        declared[name]=newid;
                }}
            }
            for(auto&s:t.outputs)if(auto*rel=std::get_if<Relation>(&s)){
                auto ls=named_members(g,rel->left,rel->span,"return relation left endpoint"),rs=named_members(g,rel->right,rel->span,"return relation right endpoint");
                for(NodeId a:ls)for(NodeId b:rs)g.add_edge({a,rel->tag,b});
            }
            NodeMap outs;
            for(NodeId i:g.ids())for(auto a:g.nodes[i].names)if(a.theory==t.universe&&a.name!=NoName){
                NameId root=root_name(a.name);
                if(visible.count(root)||explicit_roots.count(root))outs.emplace(a.name,i);
            }return {std::move(g),std::move(ins),std::move(outs)};
        }

        Graph g(names,registry);
        NodeMap ins=add_shape(g,t,NoName,t.universe,false),outs=add_shape(g,t,NoName,t.universe,true);
        for(auto[name,id]:ins)g.add_input_name(id,name);
        auto remap_value=[&](NodeMap&m,NodeId a,NodeId b,NodeId keep){
            for(auto&[k,v]:m)if(v==a||v==b)v=keep;
        };
        auto merge_maps=[&](NodeId a,NodeId b,Span span){
            NodeId keep=g.merge(a,b,span,file);
            remap_value(ins,a,b,keep);
            remap_value(outs,a,b,keep);
            return keep;
        };
        for(auto[name,a]:std::vector<std::pair<NameId,NodeId>>(ins.begin(),ins.end())){
            auto it=outs.find(name);
            if(it!=outs.end()&&g.has(a)&&g.has(it->second))merge_maps(a,it->second,t.span);
        }


        auto bind_explicit_result=[&](const Reduce&r){
            NodeMap targets=ins;
            for(auto[k,v]:outs)targets[k]=v;
            std::vector<NameId> target_names;
            std::string rr(names.str(r.ret)),pre=rr+".";
            for(auto[k,v]:targets){
                (void)v;
                std::string_view s=names.str(k);
                if(s==rr||s.starts_with(pre))target_names.push_back(k);
            }if(target_names.empty())throw Error(file,r.span,"call result '"+rr+"' has no location in the explicit return graph of '"+std::string(names.str(t.full))+"'",graph_diagnostic(g));
            std::set<NameId> matched;
            for(NameId nm:target_names){
                NodeId target=targets[nm];
                std::vector<NodeId> ids;
                for(NodeId i:g.ids())if(i!=target)for(auto a:g.nodes[i].names)if(a.name==nm){
                    ids.push_back(i);
                    break;
                }for(NodeId other:ids){
                    if(!g.has(target)||!g.has(other))continue;
                    if(g.nodes[target].types!=g.nodes[other].types)throw Error(file,r.span,"call produced variable '"+std::string(names.str(nm))+"' with incompatible type",graph_diagnostic(g));
                    target=merge_maps(target,other,r.span);
                    if(outs.count(nm))outs[nm]=target;
                    if(ins.count(nm))ins[nm]=target;
                    targets[nm]=target;
                    matched.insert(nm);
            }}
            if(targets.count(r.ret)&&!matched.count(r.ret)){
                NodeId target=targets[r.ret];
                std::vector<NodeId> produced;
                for(NodeId i:g.ids())if(i!=target){
                    bool yes=false;
                    for(auto a:g.nodes[i].names){
                        std::string_view s=names.str(a.name);
                        if(s.starts_with(pre)){
                            yes=true;
                            break;
                    }}if(yes)produced.push_back(i);
                }std::vector<NodeId> compat;
                for(NodeId i:produced)if(g.nodes[i].types==g.nodes[target].types)compat.push_back(i);
                if(compat.size()==1){
                    NodeId bound=merge_maps(target,compat[0],r.span);
                    if(outs.count(r.ret))outs[r.ret]=bound;
                    if(ins.count(r.ret))ins[r.ret]=bound;
                }else if(!produced.empty())throw Error(file,r.span,"cannot place result '"+rr+"' into explicit return variable; structured result does not match",graph_diagnostic(g));
            }
        };

        for(auto&w:t.where){
            if(auto*rel=std::get_if<Relation>(&w)){
                if (names.str(rel->tag)!="=") throw Error(file,rel->span,"invalid function where relation: function where-relations merge identities and must use '='");
                bool left_literal = is_literal(names.str(rel->left));
                bool right_literal = is_literal(names.str(rel->right));
                if (left_literal != right_literal) {
                    NameId variable = left_literal ? rel->right : rel->left;
                    NameId literal = left_literal ? rel->left : rel->right;
                    NodeId bound = bind_literal(g, t, variable, literal, rel->span);
                    if (ins.count(variable)) ins[variable] = bound;
                    if (outs.count(variable)) outs[variable] = bound;
                } else {
                    auto a=g.lookup(rel->left),b=g.lookup(rel->right);
                    if (!a||!b) throw Error(file,rel->span,"unknown where variable while applying identity relation",graph_diagnostic(g));
                    merge_maps(*a,*b,rel->span);
                }
            }else{
                const Reduce&r=std::get<Reduce>(w);
                if(r.all){
                    apply_all(g,t,r);
                    for(auto&[name,id]:ins){
                        auto x=g.lookup(name,t.universe);
                        if(x)id=*x;
                    }for(auto&[name,id]:outs){
                        auto x=g.lookup(name,t.universe);
                        if(x)id=*x;
                }}else{
                    apply_reduce(g,t,r);
                    bind_explicit_result(r);
                }
            }
        }
        return {std::move(g),std::move(ins),std::move(outs)};
    }


    void Builder::apply_reduce(Graph&g,const Type&owner,const Reduce&r,std::optional<std::set<NodeId>> selected_override,const Type*forced_fn,const std::unordered_map<NodeId,NodeId>*forced_match){
        if (!selected_override) materialize_reduce_literals(g,owner,r);
        auto live_before=g.ids();
        std::set<NodeId> before_ids(live_before.begin(),live_before.end());
        Graph before=g.induced(before_ids).first;
        auto overloads=function_overloads(r);
        if (overloads.empty()) throw Error(file,r.span,"unknown function '"+std::string(names.str(r.function))+"'",graph_diagnostic(before));
        NameId returns=names.intern("returns"),arg=names.intern("arg");
        if(!selected_override){
            for(auto&rel:r.relations){
                auto ls=named_members(g,rel.left,rel.span,"left endpoint"),rs=named_members(g,rel.right,rel.span,"right endpoint");
                for(NodeId a:ls)for(NodeId b:rs)g.add_edge({a,rel.tag,b});
        }}
        std::set<NodeId> selected_ids=selected_override?*selected_override:expand_args(g,r.args,r.span);
        auto induced=g.induced(selected_ids);
        Graph actual=std::move(induced.first);
        auto actual_map=std::move(induced.second);

        struct Valid{const Type*fn;
            Graph templ;
            NodeMap inputs,outputs;
            Graph expected;
            NodeMap expected_map;
            std::unordered_map<NodeId,NodeId> match;
            std::unordered_map<NameId,const Type*> sel;
        };
        struct Rejected {
            const Type* fn;
            std::string body;
        };
        std::vector<Valid> valid;
        std::vector<Rejected> rejected;
        for(const Type*fn:overloads){
            if(forced_fn&&fn!=forced_fn)continue;
            for(auto sel:selection_variants(*fn)){
            const size_t rejected_before = rejected.size();
            Builder cb(file,program,names,registry,sel);
            auto fb=cb.build_function(*fn);
            auto fi=cb.function_input(*fn);
            Graph expected=std::move(fi.first);
            NodeMap expected_map=std::move(fi.second);
            std::optional<std::unordered_map<NodeId,NodeId>> match;
            std::unordered_map<NodeId,std::set<NodeId>> allowed;
            if(forced_match){
                std::unordered_map<NodeId,NodeId> m;
                for(auto[eid,gid]:*forced_match){
                    auto it=actual_map.find(gid);
                    if(it!=actual_map.end())m[eid]=it->second;
                }if(m.size()==expected.ids().size())match=std::move(m);
            }
            else{
                std::vector<const Field*> fields;
                for(auto&s:fn->inputs)if(auto*f=std::get_if<Field>(&s))fields.push_back(f);
                if(fields.size()==r.args.size()){
                    for(size_t ix=0;ix<fields.size();++ix){
                        std::set<NodeId>aids;
                        for(NodeId old:expand_args(g,{r.args[ix]},r.span)){
                            auto it=actual_map.find(old);
                            if(it!=actual_map.end())aids.insert(it->second);
                        }std::string fstr(names.str(fields[ix]->name)),pre=fstr+".";
                        for(auto[name,eid]:expected_map){
                            std::string_view s=names.str(name);
                            if(s==fstr||s.starts_with(pre))allowed[eid]=aids;
                    }}match=cb.isomorphism(expected,actual,&allowed);
                }else match=cb.isomorphism(expected,actual,nullptr);
            }
            if(match&& !r.relations.empty()){
                std::unordered_map<NodeId,NodeId> inv;
                for(auto[eid,aid]:*match)inv[aid]=eid;
                std::vector<std::string> missing;
                for(auto&rel:r.relations){
                    auto lvals=expand_args(g,{rel.left},rel.span),rvals=expand_args(g,{rel.right},rel.span);
                    bool ok=false;
                    for(NodeId lg:lvals)for(NodeId rg:rvals){
                        auto al=actual_map.find(lg),ar=actual_map.find(rg);
                        if(al==actual_map.end()||ar==actual_map.end())continue;
                        auto el=inv.find(al->second),er=inv.find(ar->second);
                        if(el==inv.end()||er==inv.end())continue;
                        for(auto&e:expected.edges)if(e.left==el->second&&e.tag==rel.tag&&e.right==er->second){
                            ok=true;
                            break;
                        }if(ok)break;
                    }if(!ok)missing.push_back(std::string(names.str(rel.left))+" "+std::string(names.str(rel.tag))+" "+std::string(names.str(rel.right)));
                }if(!missing.empty()){
                    rejected.push_back({fn,"call supplies relation(s) "+join(missing,", ")+", but the function input does not declare them"});
                    match.reset();
            }}
            if(match)valid.push_back({fn,std::move(fb.graph),std::move(fb.inputs),std::move(fb.outputs),std::move(expected),std::move(expected_map),std::move(*match),std::move(sel)});
            else if(rejected.size()==rejected_before)rejected.push_back({fn,cb.mismatch(expected,actual)});
            }
        }
        if (valid.empty()) {
            std::vector<std::string> boxes;
            for (size_t i = 0; i < rejected.size(); ++i) {
                std::vector<std::string> body;
                std::stringstream ss(rejected[i].body);
                std::string line;
                while (std::getline(ss, line)) body.push_back("│ " + line);
                const Type* candidate = rejected[i].fn;
                std::string candidate_file = candidate->source_file.empty() ? file : candidate->source_file;
                std::string source = candidate_file + ":" + std::to_string(candidate->span.start.line);
                boxes.push_back("┌─ alternative " + std::to_string(i + 1) + " (" + source + ") ─\n" + join(body, "\n") + "\n└─");
            }
            throw Error(file,r.span,"cannot assign reduction result '"+std::string(names.str(r.ret))+"' from function '"+std::string(names.str(r.function))+"': none of its declared input graphs matches the selected call arguments.\n\n"+join(boxes,"\n\n"),graph_diagnostic(before));
        }
        if (valid.size()>1) throw Error(file,r.span,"ambiguous reduction for result '"+std::string(names.str(r.ret))+"': "+std::to_string(valid.size())+" declarations accept the same selected graph",graph_diagnostic(before));
        Valid v=std::move(valid.front());
        const Type&fn=*v.fn;
        Graph&templ=v.templ;
        NodeMap&inputs=v.inputs;
        NodeMap&outputs=v.outputs;
        Graph&expected=v.expected;
        NodeMap&expected_map=v.expected_map;
        auto&match=v.match;
        Builder cb(file,program,names,registry,v.sel);
        std::unordered_map<NodeId,NodeId> actual_back;
        for(auto[old,neu]:actual_map)actual_back[neu]=old;
        std::set<NodeId> output_ids;
        if(fn.return_all){
            for(NodeId i:templ.ids())output_ids.insert(i);
        }else for(auto[k,id]:outputs){
            (void)k;
            output_ids.insert(id);
        }std::unordered_map<NodeId,NodeId> concrete;
        std::set<NodeId> matched_original;
        for(NodeId eid:expected.ids()){
            NodeId orig=actual_back.at(match.at(eid));
            concrete[eid]=orig;
            matched_original.insert(orig);
        }std::set<Edge> input_edges;
        for(auto&e:expected.edges)if(!names.str(e.tag).starts_with("!"))input_edges.insert({concrete[e.left],e.tag,concrete[e.right]});
        std::unordered_map<NodeId,NodeId> eid_tid;
        std::unordered_map<NodeId,std::vector<NodeId>> eqgroups;
        for(auto[name,eid]:expected_map){
            auto it=inputs.find(name);
            if(it!=inputs.end()){
                eid_tid[eid]=it->second;
                eqgroups[it->second].push_back(concrete[eid]);
        }}

        std::unordered_map<NodeId,std::pair<NodeId,NodeId>> promoted;
        std::unordered_map<NodeId,std::set<NodeId>> promoted_operands;
        std::set<NodeId> residual_calls,operand_keep;
        std::set<Edge> preserve_edges;
        if(selected_override){
            for(auto[ceid,cgid]:concrete){
                auto ti=eid_tid.find(ceid);
                if(ti==eid_tid.end()||output_ids.count(ti->second)||!call_name_id(g,cgid))continue;
                std::vector<Edge> extra;
                for(auto&e:g.edges)if(e.tag==arg&&e.right==cgid&&!input_edges.count(e)&&!matched_original.count(e.left))extra.push_back(e);
                if(extra.empty())continue;
                std::vector<Edge> rets;
                for(auto&e:expected.edges)if(!names.str(e.tag).starts_with("!")&&e.left==ceid&&e.tag==returns)rets.push_back(e);
                if(rets.size()!=1)throw Error(file,r.span,"reduction cannot preserve variadic context: matched call needs exactly one returns edge",graph_diagnostic(before));
                NodeId rgid=concrete.at(rets[0].right);
                std::vector<Edge> args;
                for(auto&e:expected.edges)if(!names.str(e.tag).starts_with("!")&&e.right==ceid&&e.tag==arg)args.push_back(e);
                std::set<NodeId> tids;
                for(auto&e:args){
                    auto q=eid_tid.find(e.left);
                    if(q!=eid_tid.end()&&output_ids.count(q->second))tids.insert(q->second);
                }if(tids.size()!=1)throw Error(file,r.span,"reduction cannot lift variadic context: exactly one returned input identity must remain",graph_diagnostic(before));
                NodeId ptid=*tids.begin();
                if(promoted.count(ptid)&&promoted[ptid].first!=rgid)throw Error(file,r.span,"reduction has conflicting residual variadic outputs",graph_diagnostic(before));
                std::set<NodeId> ops;
                for(auto&e:args){
                    auto q=eid_tid.find(e.left);
                    if(q!=eid_tid.end()&&q->second==ptid)ops.insert(concrete[e.left]);
                }promoted[ptid]={rgid,cgid};
                promoted_operands[ptid].insert(ops.begin(),ops.end());
                residual_calls.insert(cgid);
                preserve_edges.insert({cgid,returns,rgid});
                for(NodeId x:ops)preserve_edges.insert({x,arg,cgid});
        }}
        if(selected_override){
            std::vector<Edge> kept;
            for(auto&e:g.edges)if(!input_edges.count(e)||preserve_edges.count(e))kept.push_back(e);
            g.edges=std::move(kept);
        }

        std::unordered_map<NodeId,NodeId> redirect;
        for(NodeId i:matched_original)redirect[i]=i;
        std::function<NodeId(NodeId)> resolve=[&](NodeId x){
            while(redirect.count(x)&&redirect[x]!=x)x=redirect[x];
            return x;
        };
        auto merge_into=[&](NodeId a,NodeId b){
            a=resolve(a);
            b=resolve(b);
            if(a==b)return a;
            NodeId keep=g.merge(a,b,r.span,file);
            redirect[b]=keep;
            selected_ids.erase(b);
            selected_ids.insert(keep);
            return keep;
        };
        std::unordered_map<NodeId,NodeId> mapping;
        for(auto&[tid,gids0]:eqgroups){
            std::vector<NodeId> gids;
            for(NodeId x:gids0)if(std::find(gids.begin(),gids.end(),x)==gids.end())gids.push_back(x);
            if(promoted.count(tid)){
                NodeId gid=resolve(promoted[tid].first);
                for(NodeId other:gids){
                    other=resolve(other);
                    bool operand=false;
                    for(NodeId x:promoted_operands[tid])if(resolve(x)==other){
                        operand=true;
                        break;
                    }if(other!=gid&&!operand&&g.has(other))gid=merge_into(gid,other);
                }mapping[tid]=gid;
            }else{
                std::vector<NodeId> live;
                for(NodeId x:gids){
                    x=resolve(x);
                    if(g.has(x)&&std::find(live.begin(),live.end(),x)==live.end())live.push_back(x);
                }if(live.empty())continue;
                NodeId gid=live[0];
                for(size_t k=1;k<live.size();++k)gid=merge_into(gid,live[k]);
                mapping[tid]=gid;
        }}
        for(auto&[tid,p]:promoted){
            p.first=resolve(p.first);
            p.second=resolve(p.second);
            mapping[tid]=p.first;
            for(NodeId x:promoted_operands[tid])operand_keep.insert(resolve(x));
        }
        if(fn.return_all){
            for(auto[tid,gid]:mapping)if(g.has(gid))for(auto a:templ.nodes[tid].names)g.add_alias(gid,a.theory,a.name);
        }
        std::unordered_map<NodeId,NodeId> created;
        for(auto[name,tid]:outputs){
            NameId full=names.intern(std::string(names.str(r.ret))+"."+std::string(names.str(name)));
            NodeId gid;
            if(mapping.count(tid))gid=mapping[tid];
            else if(created.count(tid))gid=created[tid];
            else{
                gid=g.add_node(templ.nodes[tid].types,owner.universe,full);
                created[tid]=gid;
            }g.add_alias(gid,owner.universe,full);
            for (const Alias& alias : templ.nodes[tid].names) {
                if (is_literal(names.str(alias.name))) {
                    g.add_alias(gid, alias.theory, alias.name);
                }
            }
            created[tid]=gid;
        }
        std::unordered_map<NodeId,NodeId> allmap=mapping;
        for(auto[k,vv]:created)allmap[k]=vv;
        if(fn.return_all){
            for(NodeId tid:templ.ids()){
                if(!allmap.count(tid)){
                    NodeId id=g.add_node(templ.nodes[tid].types);
                    allmap[tid]=id;
                }for(auto a:templ.nodes[tid].names)g.add_alias(allmap[tid],a.theory,a.name);
        }}
        if(!selected_override){
            std::unordered_map<NodeId,NameId> rev;
            for(auto[name,eid]:expected_map)rev[eid]=name;
            for(auto&e:expected.edges)if(!names.str(e.tag).starts_with("!")){
                NameId ln=rev.at(e.left),rn=rev.at(e.right);
                NodeId lt=inputs.at(ln),rt=inputs.at(rn);
                if(allmap.count(lt)&&allmap.count(rt))g.add_edge({allmap[lt],e.tag,allmap[rt]});
        }}
        if(fn.return_all){
            for(auto&e:templ.edges)if(!names.str(e.tag).starts_with("!")&&allmap.count(e.left)&&allmap.count(e.right))g.add_edge({allmap[e.left],e.tag,allmap[e.right]});
        }else{
            Graph outg(names,registry);
            NodeMap outnames=cb.add_shape(outg,fn,NoName,fn.universe,true);
            std::unordered_map<NodeId,NodeId> outrev;
            for(auto[name,oid]:outnames)outrev[oid]=outputs.at(name);
            for(auto&e:outg.edges)if(allmap.count(outrev[e.left])&&allmap.count(outrev[e.right]))g.add_edge({allmap[outrev[e.left]],e.tag,allmap[outrev[e.right]]});
        }
        std::set<NodeId> matched;
        for(NodeId i:matched_original){
            NodeId q=resolve(i);
            if(g.has(q))matched.insert(q);
        }std::set<NodeId> protected_ids;
        for(NodeId tid:output_ids)if(allmap.count(tid)&&g.has(allmap[tid]))protected_ids.insert(allmap[tid]);
        if(selected_override){
            for(NodeId i:residual_calls){
                i=resolve(i);
                if(g.has(i))protected_ids.insert(i);
            }for(NodeId i:operand_keep)if(g.has(i))protected_ids.insert(i);
            for(auto&e:g.edges){
                std::string_view tag=names.str(e.tag);
                if(tag.starts_with("!"))continue;
                if(matched.count(e.left)&&!matched.count(e.right))protected_ids.insert(e.left);
                if(matched.count(e.right)&&!matched.count(e.left))protected_ids.insert(e.right);
        }}
        std::set<NodeId> remove;
        for(NodeId i:matched)if(!protected_ids.count(i))remove.insert(i);
        if(!remove.empty())g.remove_component(remove,protected_ids,file,r.span,matched);
        std::vector<std::string> lost;
        for(auto[name,tid]:outputs)if(!allmap.count(tid)||!g.has(allmap[tid]))lost.push_back(std::string(names.str(name)));
        if(!lost.empty())throw Error(file,r.span,"reduction would drop output node(s): "+join(lost,", "),graph_diagnostic(before));
    }


    static Graph merge_graphs(const Graph&a,const Graph&b){
        Graph g(*a.names,*a.types);
        for(const Graph*src:{&a,&b}){
            std::unordered_map<NodeId,NodeId> m;
            for(NodeId old:src->ids()){
                NodeId id=g.add_node(src->nodes[old].types);
                g.nodes[id].names=src->nodes[old].names;
                g.nodes[id].input_names=src->nodes[old].input_names;
                m[old]=id;
            }for(auto&e:src->edges)if(src->has(e.left)&&src->has(e.right))g.add_edge({m[e.left],e.tag,m[e.right]});
        }return g;
    }

    static std::vector<Graph> build_variants(const std::string&file,Program&program,const Type&t,Interner&names,TypeRegistry&registry){
        std::unordered_map<NameId,std::vector<const Type*>> groups;
        for(auto&u:program.universes)for(auto&d:u.types)if(!d.union_template)groups[d.full].push_back(&d);
        std::function<std::vector<Graph>(const Type&,NameId,std::vector<NameId>)> expand;
        expand=[&](const Type&d,NameId prefix,std::vector<NameId> stack)->std::vector<Graph>{if(std::find(stack.begin(),stack.end(),d.full)!=stack.end())throw Error(file,d.span,"recursive expansion through "+std::string(names.str(d.full)));
            const auto&ss=d.function()?d.outputs:d.inputs;
            if(ss.size()==1){
                if(auto*x=std::get_if<Field>(&ss[0]);x&&names.str(x->name)=="$"){
                    NameId ref=x->types.at(0);
                    std::vector<Graph> out;
                    auto it=groups.find(ref);
                    if(it==groups.end())throw Error(file,x->span,"unknown type '"+std::string(names.str(ref))+"'");
                    for(const Type*alt:it->second){
                        if(alt->inputs.empty()&&!alt->function()){
                            Graph g(names,registry);
                            NameId nm=(prefix==NoName||names.str(prefix).empty())?d.name:prefix;
                            g.add_node(registry.bit(ref),d.universe,nm);
                            out.push_back(std::move(g));
                        }else{
                            auto st=stack;
                            st.push_back(d.full);
                            auto xs=expand(*alt,prefix,std::move(st));
                            out.insert(out.end(),std::make_move_iterator(xs.begin()),std::make_move_iterator(xs.end()));
                    }}return out;
            }}
            std::vector<Graph> variants{Graph(names,registry)};
            for(auto&s:ss){
                auto*x=std::get_if<Field>(&s);
                if(!x)continue;
                NameId nm=(prefix==NoName||names.str(prefix).empty())?x->name:names.intern(std::string(names.str(prefix))+"."+std::string(names.str(x->name)));
                std::vector<Graph> field_choices;
                for(NameId ref:x->types){
                    auto it=groups.find(ref);
                    if(it==groups.end())throw Error(file,x->span,"unknown type '"+std::string(names.str(ref))+"'");
                    for(const Type*alt:it->second){
                        if(alt->union_template)continue;
                        if(alt->inputs.empty()&&!alt->function()){
                            Graph q(names,registry);
                            q.add_node(registry.bit(ref),d.universe,nm);
                            field_choices.push_back(std::move(q));
                        }else{
                            auto st=stack;
                            st.push_back(d.full);
                            auto children=expand(*alt,nm,std::move(st));
                            field_choices.insert(field_choices.end(),
                                std::make_move_iterator(children.begin()),
                                std::make_move_iterator(children.end()));
                        }
                    }
                }
                std::vector<Graph> next;
                for(auto&a:variants)for(auto&b:field_choices)next.push_back(merge_graphs(a,b));
                variants=std::move(next);
            }
            for (auto& g:variants) {
                for (auto& s:ss) {
                    if (auto* r=std::get_if<Relation>(&s)) {
                        NameId l=(prefix==NoName||names.str(prefix).empty())?r->left:names.intern(std::string(names.str(prefix))+"."+std::string(names.str(r->left)));
                        NameId rr=(prefix==NoName||names.str(prefix).empty())?r->right:names.intern(std::string(names.str(prefix))+"."+std::string(names.str(r->right)));
                        auto a=g.lookup(l),b=g.lookup(rr);
                        if (!a||!b) throw Error(file,r->span,"relation refers to a name that does not resolve to a leaf in this alternative");
                        g.add_edge({*a,r->tag,*b});
                    }
                }
            }
            return variants;
        };
        std::vector<Graph> out=expand(t,NoName,{});
        Builder builder(file,program,names,registry);
        for(auto&g:out){
            for(NodeId i:g.ids())for(auto a:g.nodes[i].names)if(a.name!=NoName)g.add_input_name(i,a.name);
            for(auto&w:t.where){
                if(auto*r=std::get_if<Relation>(&w)){
                    if(names.str(r->tag)!="=")throw Error(file,r->span,"where relations merge identities and must use '='");
                    auto a=g.lookup(r->left),b=g.lookup(r->right);
                    if(!a||!b)throw Error(file,r->span,"where merge cannot be resolved in this alternative");
                    g.merge(*a,*b,r->span,file);
                }else{
                    auto&red=std::get<Reduce>(w);
                    if(red.all)builder.apply_all(g,t,red);
                    else builder.apply_reduce(g,t,red);
        }}}
        std::vector<Graph> uniq;
        std::set<std::string> seen;
        for(auto&g:out){
            std::vector<std::string> ns,es;
            for(NodeId i:g.ids())ns.push_back(best_name(g,i)+":"+std::to_string(g.nodes[i].types));
            for(auto&e:g.edges)es.push_back(best_name(g,e.left)+":"+std::string(names.str(e.tag))+":"+best_name(g,e.right));
            std::sort(ns.begin(),ns.end());
            std::sort(es.begin(),es.end());
            std::string sig=join(ns,";")+"|"+join(es,";");
            if(seen.insert(sig).second)uniq.push_back(std::move(g));
        }return uniq;
    }

    static std::unordered_map<NodeId,NameId> graph_names(const Graph&g,const std::optional<std::set<NodeId>>&subset=std::nullopt){
        std::set<NodeId> ids;
        if(subset)ids=*subset;
        else for(NodeId i:g.ids())ids.insert(i);
        std::unordered_map<NodeId,std::vector<NameId>> cand;
        std::unordered_map<NameId,size_t> counts;
        for(NodeId i:ids){
            auto xs=g.nodes[i].input_names;
            if(xs.empty())xs=node_aliases(g,i);
            std::sort(xs.begin(),xs.end(),[&](NameId a,NameId b){
                auto sa=g.names->str(a),sb=g.names->str(b);return std::pair{sa.size(),sa}<std::pair{sb.size(),sb};});
            cand[i]=xs;
            for(NameId x:xs)++counts[x];
        }std::unordered_map<NodeId,NameId> out;
        for(NodeId i:ids){
            NameId pick=NoName;
            for(NameId x:cand[i])if(counts[x]==1){
                pick=x;
                break;
            }if(pick==NoName&&!cand[i].empty())pick=cand[i][0];
            out[i]=pick;
        }return out;
    }
    static void print_node(const Graph&g,NodeId i,int indent=0,NameId display=NoName){
        std::string nm=display==NoName?best_name(g,i):std::string(g.names->str(display));
        std::vector<std::string> ts;
        for(NameId t:g.types->names_for(g.nodes[i].types))ts.push_back(typefmt(g.names->str(t)));
        std::cout<<std::string(indent,' ')<<cyan(nm)<<" "<<gray(":")<<" "<<join(ts,gray(" | "))<<"\n";
    }
    static void print_edge(const Graph&g,const Edge&e,int indent,const std::unordered_map<NodeId,NameId>&gn){
        auto show=[&](NodeId i){
            auto it=gn.find(i);
            return it==gn.end()||it->second==NoName?"#"+std::to_string(i):std::string(g.names->str(it->second));
        };
        std::cout<<std::string(indent,' ')<<cyan(show(e.left))<<" "<<ansi::BOLD<<g.names->str(e.tag)<<ansi::RESET<<" "<<cyan(show(e.right))<<"\n";
    }
    static void print_graph(const Graph&g,int indent=0,std::optional<std::set<NodeId>> subset=std::nullopt){
        std::set<NodeId> ids;
        if(subset)ids=*subset;
        else for(NodeId i:g.ids())ids.insert(i);
        if(ids.empty()){
            std::cout<<std::string(indent,' ')<<gray("∅")<<"\n";
            return;
        }auto gn=graph_names(g,ids);
        std::vector<NodeId> order(ids.begin(),ids.end());
        std::sort(order.begin(),order.end(),[&](NodeId a,NodeId b){
            std::string sa=gn[a]==NoName?"#"+std::to_string(a):std::string(g.names->str(gn[a]));std::string sb=gn[b]==NoName?"#"+std::to_string(b):std::string(g.names->str(gn[b]));return sa<sb;});
        for(NodeId i:order)print_node(g,i,indent,gn[i]);
        for(auto&e:g.edges)if(ids.count(e.left)&&ids.count(e.right))print_edge(g,e,indent,gn);
    }

    enum class HeapTag : uint8_t {
        None = 0,
        String = 1,
        NatArray = 2,
        FloatArray = 3,
        StringArray = 4,
        ArrayArray = 5
    };

    static constexpr uint64_t HANDLE_ID_BITS = 20;
    static constexpr uint64_t HANDLE_SIZE_BITS = 20;
    static constexpr uint64_t HANDLE_START_BITS = 20;
    static constexpr uint64_t HANDLE_TAG_BITS = 4;
    static constexpr uint64_t HANDLE_ID_MASK = (uint64_t{1} << HANDLE_ID_BITS) - 1;
    static constexpr uint64_t HANDLE_SIZE_MASK = (uint64_t{1} << HANDLE_SIZE_BITS) - 1;
    static constexpr uint64_t HANDLE_START_MASK = (uint64_t{1} << HANDLE_START_BITS) - 1;
    static constexpr uint64_t HANDLE_SIZE_SHIFT = HANDLE_ID_BITS;
    static constexpr uint64_t HANDLE_START_SHIFT = HANDLE_ID_BITS + HANDLE_SIZE_BITS;
    static constexpr uint64_t HANDLE_TAG_SHIFT = HANDLE_ID_BITS + HANDLE_SIZE_BITS + HANDLE_START_BITS;
    static constexpr uint64_t MAX_PLAIN_NAT = (uint64_t{1} << HANDLE_TAG_SHIFT) - 1;

    static HeapTag heap_tag(uint64_t value) {
        return static_cast<HeapTag>((value >> HANDLE_TAG_SHIFT) & ((uint64_t{1} << HANDLE_TAG_BITS) - 1));
    }

    static uint64_t heap_start(uint64_t value) {
        return (value >> HANDLE_START_SHIFT) & HANDLE_START_MASK;
    }

    static uint64_t heap_size(uint64_t value) {
        return (value >> HANDLE_SIZE_SHIFT) & HANDLE_SIZE_MASK;
    }

    static uint32_t heap_id(uint64_t value) {
        return static_cast<uint32_t>(value & HANDLE_ID_MASK);
    }

    static bool is_array_tag(HeapTag tag) {
        return tag == HeapTag::NatArray || tag == HeapTag::FloatArray || tag == HeapTag::StringArray || tag == HeapTag::ArrayArray;
    }

    static bool is_managed_array_tag(HeapTag tag) {
        return tag == HeapTag::StringArray || tag == HeapTag::ArrayArray;
    }

    static uint64_t make_heap_handle(HeapTag tag, uint64_t start, uint64_t size, uint64_t id) {
        if (tag == HeapTag::None) throw std::runtime_error("cannot create a heap handle with tag 0");
        if (start > HANDLE_START_MASK) throw std::runtime_error("heap object start does not fit in the 20-bit start field");
        if (size > HANDLE_SIZE_MASK) throw std::runtime_error("heap object size does not fit in the 20-bit size field");
        if (id > HANDLE_ID_MASK) throw std::runtime_error("heap object id does not fit in the 20-bit id field");
        if (start + size > HANDLE_START_MASK + uint64_t{1}) throw std::runtime_error("heap view end exceeds the 20-bit addressable range");
        return (static_cast<uint64_t>(tag) << HANDLE_TAG_SHIFT)
            | (start << HANDLE_START_SHIFT)
            | (size << HANDLE_SIZE_SHIFT)
            | id;
    }

    struct Value {
        enum class Tag : uint8_t {
            Nat,
            Float
        };

        Tag tag = Tag::Nat;
        union Storage {
            uint64_t nat;
            double floating;
            Storage() : nat(0) {}
        } data;

        Value() {
            data.nat = 0;
        }

        static Value nat(uint64_t value) {
            Value out;
            out.tag = Tag::Nat;
            out.data.nat = value;
            return out;
        }

        static Value floating(double value) {
            Value out;
            out.tag = Tag::Float;
            out.data.floating = value;
            return out;
        }
    };

    struct RuntimeMemory {
        struct ArrayMeta {
            uint64_t words = 0;
            uint32_t refs = 0;
        };

        std::vector<std::string*> strings;
        std::vector<uint32_t> string_refs;

        // All arrays use flat uint64_t storage. Float elements are their IEEE-754 bits.
        // ArrayArray elements are NatArray handles. StringArray elements are String handles.
        std::vector<uint64_t*> arrays;
        std::vector<ArrayMeta> array_meta;

        ~RuntimeMemory() {
            for (uint64_t* array : arrays) std::free(array);
            for (std::string* string : strings) delete string;
        }

        static bool is_handle(uint64_t value) {
            HeapTag tag = heap_tag(value);
            return tag == HeapTag::String || is_array_tag(tag);
        }

        uint64_t allocate_string(std::string value) {
            if (value.size() > HANDLE_SIZE_MASK) throw std::runtime_error("string is too large for a 20-bit handle size");
            if (strings.size() > HANDLE_ID_MASK) throw std::runtime_error("string id overflow: more than 2^20 string allocations are recorded");
            uint32_t id = static_cast<uint32_t>(strings.size());
            strings.push_back(new std::string(std::move(value)));
            string_refs.push_back(1);
            return make_heap_handle(HeapTag::String, 0, strings.back()->size(), id);
        }

        void checked_string(uint64_t handle) const {
            if (heap_tag(handle) != HeapTag::String) throw std::runtime_error("value is not a string handle");
            uint32_t id = heap_id(handle);
            if (id >= strings.size()) throw std::runtime_error("string id " + std::to_string(id) + " is out of bounds");
            if (!strings[id] || string_refs[id] == 0) throw std::runtime_error("string id " + std::to_string(id) + " has already been released");
            uint64_t start = heap_start(handle);
            uint64_t size = heap_size(handle);
            if (start > strings[id]->size() || size > strings[id]->size() - start) {
                throw std::runtime_error("string view [" + std::to_string(start) + ", " + std::to_string(start + size) + ") is out of bounds for allocation " + std::to_string(id));
            }
        }

        std::string_view string_view(uint64_t handle) const {
            checked_string(handle);
            const std::string& string = *strings[heap_id(handle)];
            return std::string_view(string.data() + heap_start(handle), static_cast<size_t>(heap_size(handle)));
        }

        uint64_t slice_string(uint64_t handle, uint64_t start, uint64_t size) {
            checked_string(handle);
            uint64_t parent_size = heap_size(handle);
            if (start > parent_size || size > parent_size - start) {
                throw std::runtime_error("string slice [" + std::to_string(start) + ", " + std::to_string(start + size) + ") is out of bounds for a string of size " + std::to_string(parent_size));
            }
            retain(handle);
            return make_heap_handle(HeapTag::String, heap_start(handle) + start, size, heap_id(handle));
        }

        ArrayMeta checked_array_meta(uint64_t handle, std::optional<HeapTag> expected = std::nullopt) const {
            HeapTag tag = heap_tag(handle);
            if (!is_array_tag(tag)) throw std::runtime_error("value is not an array handle");
            if (expected && tag != *expected) throw std::runtime_error("array has the wrong element view type");
            uint32_t id = heap_id(handle);
            if (id >= arrays.size()) throw std::runtime_error("array id " + std::to_string(id) + " is out of bounds");
            const ArrayMeta& meta = array_meta[id];
            if (!arrays[id] || meta.refs == 0) throw std::runtime_error("array id " + std::to_string(id) + " has already been released");
            uint64_t start = heap_start(handle);
            uint64_t size = heap_size(handle);
            if (start > meta.words || size > meta.words - start) {
                throw std::runtime_error("array view [" + std::to_string(start) + ", " + std::to_string(start + size) + ") is out of bounds for allocation " + std::to_string(id));
            }
            return meta;
        }

        const uint64_t* array_data(uint64_t handle, std::optional<HeapTag> expected = std::nullopt) const {
            checked_array_meta(handle, expected);
            return arrays[heap_id(handle)] + heap_start(handle);
        }

        uint64_t* array_data(uint64_t handle, std::optional<HeapTag> expected = std::nullopt) {
            checked_array_meta(handle, expected);
            return arrays[heap_id(handle)] + heap_start(handle);
        }

        void validate_managed_elements(HeapTag tag, const uint64_t* data, uint64_t size, std::optional<uint32_t> forbidden_array_id = std::nullopt) const {
            if (tag == HeapTag::StringArray) {
                for (uint64_t i = 0; i < size; ++i) checked_string(data[i]);
                return;
            }
            if (tag == HeapTag::ArrayArray) {
                for (uint64_t i = 0; i < size; ++i) {
                    if (!is_array_tag(heap_tag(data[i]))) throw std::runtime_error("aarray elements must be array handles");
                    checked_array_meta(data[i]);
                    if (forbidden_array_id && heap_id(data[i]) == *forbidden_array_id) {
                        throw std::runtime_error("aarray cannot contain a view of its own backing allocation");
                    }
                }
            }
        }

        void retain_view_children(HeapTag tag, const uint64_t* data, uint64_t size, std::optional<uint32_t> forbidden_array_id = std::nullopt) {
            if (!is_managed_array_tag(tag)) return;
            validate_managed_elements(tag, data, size, forbidden_array_id);
            uint64_t done = 0;
            try {
                for (; done < size; ++done) retain(data[done]);
            } catch (...) {
                while (done > 0) {
                    --done;
                    release(data[done]);
                }
                throw;
            }
        }

        void release_view_children(HeapTag tag, const uint64_t* data, uint64_t size) {
            if (!is_managed_array_tag(tag)) return;
            for (uint64_t i = 0; i < size; ++i) release(data[i]);
        }

        uint64_t allocate_array(HeapTag tag, const std::vector<uint64_t>& values) {
            if (!is_array_tag(tag)) throw std::runtime_error("invalid array heap tag");
            if (values.size() > HANDLE_SIZE_MASK) throw std::runtime_error("array is too large for a 20-bit handle size");
            if (arrays.size() > HANDLE_ID_MASK) throw std::runtime_error("array id overflow: more than 2^20 array allocations are recorded");

            uint32_t id = static_cast<uint32_t>(arrays.size());
            retain_view_children(tag, values.data(), values.size(), id);

            size_t allocation_words = std::max<size_t>(1, values.size());
            uint64_t* data = static_cast<uint64_t*>(std::malloc(allocation_words * sizeof(uint64_t)));
            if (!data) {
                release_view_children(tag, values.data(), values.size());
                throw std::bad_alloc();
            }
            if (!values.empty()) std::copy(values.begin(), values.end(), data);
            arrays.push_back(data);
            array_meta.push_back({static_cast<uint64_t>(values.size()), 1});
            return make_heap_handle(tag, 0, values.size(), id);
        }

        uint64_t retain_array_as(uint64_t handle, HeapTag wanted) {
            checked_array_meta(handle);
            if (!is_array_tag(wanted)) throw std::runtime_error("invalid array cast target");
            const uint64_t* data = array_data(handle);
            retain_view_children(wanted, data, heap_size(handle), heap_id(handle));
            uint32_t id = heap_id(handle);
            if (array_meta[id].refs == std::numeric_limits<uint32_t>::max()) {
                release_view_children(wanted, data, heap_size(handle));
                throw std::runtime_error("array reference count overflow");
            }
            ++array_meta[id].refs;
            return make_heap_handle(wanted, heap_start(handle), heap_size(handle), id);
        }

        uint64_t slice_array_as(uint64_t handle, HeapTag wanted, uint64_t start, uint64_t size) {
            checked_array_meta(handle);
            if (!is_array_tag(wanted)) throw std::runtime_error("invalid array slice target");
            uint64_t parent_size = heap_size(handle);
            if (start > parent_size || size > parent_size - start) {
                throw std::runtime_error("array slice [" + std::to_string(start) + ", " + std::to_string(start + size) + ") is out of bounds for an array of size " + std::to_string(parent_size));
            }
            uint64_t out = make_heap_handle(wanted, heap_start(handle) + start, size, heap_id(handle));
            const uint64_t* data = array_data(out);
            retain_view_children(wanted, data, size, heap_id(handle));
            uint32_t id = heap_id(handle);
            if (array_meta[id].refs == std::numeric_limits<uint32_t>::max()) {
                release_view_children(wanted, data, size);
                throw std::runtime_error("array reference count overflow");
            }
            ++array_meta[id].refs;
            return out;
        }

        bool can_reuse_array(uint64_t handle) const {
            if (!is_array_tag(heap_tag(handle))) return false;
            checked_array_meta(handle);
            return array_meta[heap_id(handle)].refs == 1;
        }

        bool can_reuse_string(uint64_t handle) const {
            if (heap_tag(handle) != HeapTag::String) return false;
            checked_string(handle);
            return string_refs[heap_id(handle)] == 1;
        }

        uint64_t replace_array(uint64_t old_handle, HeapTag new_tag, const std::vector<uint64_t>& values) {
            if (!can_reuse_array(old_handle)) throw std::runtime_error("attempted to reuse a shared array allocation");
            if (!is_array_tag(new_tag)) throw std::runtime_error("invalid replacement array tag");
            if (values.size() > HANDLE_SIZE_MASK) throw std::runtime_error("array is too large for a 20-bit handle size");

            uint32_t id = heap_id(old_handle);
            const uint64_t* old_view = array_data(old_handle);
            HeapTag old_tag = heap_tag(old_handle);
            uint64_t old_size = heap_size(old_handle);

            retain_view_children(new_tag, values.data(), values.size(), id);
            // realloc may move the old storage, so preserve managed child handles first.
            std::vector<uint64_t> old_children;
            if (is_managed_array_tag(old_tag)) {
                old_children.assign(old_view, old_view + old_size);
            }
            size_t allocation_words = std::max<size_t>(1, values.size());
            void* resized = std::realloc(arrays[id], allocation_words * sizeof(uint64_t));
            if (!resized) {
                release_view_children(new_tag, values.data(), values.size());
                throw std::bad_alloc();
            }

            arrays[id] = static_cast<uint64_t*>(resized);
            if (is_managed_array_tag(old_tag)) release_view_children(old_tag, old_children.data(), old_children.size());
            if (!values.empty()) std::copy(values.begin(), values.end(), arrays[id]);
            array_meta[id].words = values.size();
            return make_heap_handle(new_tag, 0, values.size(), id);
        }

        uint64_t replace_string(uint64_t old_handle, std::string value) {
            if (!can_reuse_string(old_handle)) throw std::runtime_error("attempted to reuse a shared string allocation");
            if (value.size() > HANDLE_SIZE_MASK) throw std::runtime_error("string is too large for a 20-bit handle size");
            uint32_t id = heap_id(old_handle);
            strings[id]->assign(std::move(value));
            return make_heap_handle(HeapTag::String, 0, strings[id]->size(), id);
        }

        void* pointer_for(uint64_t handle) const {
            switch (heap_tag(handle)) {
                case HeapTag::String:
                    checked_string(handle);
                    return strings[heap_id(handle)]->data() + heap_start(handle);
                case HeapTag::NatArray:
                case HeapTag::FloatArray:
                case HeapTag::StringArray:
                case HeapTag::ArrayArray:
                    checked_array_meta(handle);
                    return arrays[heap_id(handle)] + heap_start(handle);
                default:
                    return nullptr;
            }
        }

        void retain(uint64_t handle) {
            switch (heap_tag(handle)) {
                case HeapTag::String: {
                    checked_string(handle);
                    uint32_t id = heap_id(handle);
                    if (string_refs[id] == std::numeric_limits<uint32_t>::max()) throw std::runtime_error("string reference count overflow");
                    ++string_refs[id];
                    break;
                }
                case HeapTag::NatArray:
                case HeapTag::FloatArray:
                case HeapTag::StringArray:
                case HeapTag::ArrayArray: {
                    checked_array_meta(handle);
                    const uint64_t* data = array_data(handle);
                    retain_view_children(heap_tag(handle), data, heap_size(handle), heap_id(handle));
                    uint32_t id = heap_id(handle);
                    if (array_meta[id].refs == std::numeric_limits<uint32_t>::max()) {
                        release_view_children(heap_tag(handle), data, heap_size(handle));
                        throw std::runtime_error("array reference count overflow");
                    }
                    ++array_meta[id].refs;
                    break;
                }
                default:
                    throw std::runtime_error("cannot retain a non-heap natural value");
            }
        }

        void release(uint64_t handle) {
            switch (heap_tag(handle)) {
                case HeapTag::String: {
                    uint32_t id = heap_id(handle);
                    if (id >= strings.size()) throw std::runtime_error("string id " + std::to_string(id) + " is out of bounds");
                    if (!strings[id] || string_refs[id] == 0) throw std::runtime_error("string id " + std::to_string(id) + " has already been released");
                    if (--string_refs[id] == 0) {
                        delete strings[id];
                        strings[id] = nullptr;
                    }
                    break;
                }
                case HeapTag::NatArray:
                case HeapTag::FloatArray:
                case HeapTag::StringArray:
                case HeapTag::ArrayArray: {
                    uint32_t id = heap_id(handle);
                    if (id >= arrays.size()) throw std::runtime_error("array id " + std::to_string(id) + " is out of bounds");
                    ArrayMeta& meta = array_meta[id];
                    if (!arrays[id] || meta.refs == 0) throw std::runtime_error("array id " + std::to_string(id) + " has already been released");
                    const uint64_t* data = arrays[id] + heap_start(handle);
                    release_view_children(heap_tag(handle), data, heap_size(handle));
                    if (--meta.refs == 0) {
                        std::free(arrays[id]);
                        arrays[id] = nullptr;
                        meta.words = 0;
                    }
                    break;
                }
                default:
                    throw std::runtime_error("cannot release a non-heap natural value");
            }
        }
    };

    static bool value_is_handle(const Value& value) {
        return value.tag == Value::Tag::Nat && RuntimeMemory::is_handle(value.data.nat);
    }

    static uint64_t plain_nat(const Value& value) {
        if (value.tag != Value::Tag::Nat) throw std::runtime_error("natural-number builtin received a float");
        if (heap_tag(value.data.nat) != HeapTag::None) throw std::runtime_error("natural-number builtin received an enclosed string/array handle");
        if (value.data.nat > MAX_PLAIN_NAT) throw std::runtime_error("natural number exceeds the tag-0 60-bit range");
        return value.data.nat;
    }

    static double float_number(const Value& value) {
        if (value.tag != Value::Tag::Float) throw std::runtime_error("float builtin received a natural/string/array value");
        return value.data.floating;
    }

    static uint64_t string_handle(const Value& value, const RuntimeMemory& memory) {
        if (value.tag != Value::Tag::Nat || heap_tag(value.data.nat) != HeapTag::String) {
            throw std::runtime_error("string builtin received a non-string value");
        }
        memory.checked_string(value.data.nat);
        return value.data.nat;
    }

    static uint64_t array_handle(const Value& value, const RuntimeMemory& memory) {
        if (value.tag != Value::Tag::Nat || !is_array_tag(heap_tag(value.data.nat))) {
            throw std::runtime_error("array builtin received a non-array value");
        }
        memory.checked_array_meta(value.data.nat);
        return value.data.nat;
    }

    static uint64_t checked_nat_add(uint64_t a, uint64_t b) {
        if (b > MAX_PLAIN_NAT || a > MAX_PLAIN_NAT - b) throw std::runtime_error("natural addition overflow");
        return a + b;
    }

    static uint64_t checked_nat_mul(uint64_t a, uint64_t b) {
        if (a != 0 && b > MAX_PLAIN_NAT / a) throw std::runtime_error("natural multiplication overflow");
        return a * b;
    }

    static uint64_t checked_nat_pow(uint64_t base, uint64_t exponent) {
        uint64_t result = 1;
        while (exponent) {
            if (exponent & 1) result = checked_nat_mul(result, base);
            exponent >>= 1;
            if (exponent) base = checked_nat_mul(base, base);
        }
        return result;
    }

    static std::string value_string(const Value& value, const RuntimeMemory& memory);

    static std::string array_string(uint64_t handle, const RuntimeMemory& memory) {
        RuntimeMemory::ArrayMeta meta = memory.checked_array_meta(handle);
        (void)meta;
        const uint64_t* data = memory.array_data(handle);
        uint64_t size = heap_size(handle);
        HeapTag tag = heap_tag(handle);
        std::ostringstream out;
        out << '[';
        for (uint64_t i = 0; i < size; ++i) {
            if (i) out << ", ";
            if (tag == HeapTag::NatArray) {
                if (heap_tag(data[i]) == HeapTag::None) out << data[i];
                else out << "0x" << std::hex << data[i] << std::dec;
            } else if (tag == HeapTag::FloatArray) {
                double element = std::bit_cast<double>(data[i]);
                out << std::setprecision(15) << element;
            } else if (tag == HeapTag::StringArray) {
                out << '"';
                for (char c : memory.string_view(data[i])) {
                    if (c == '\\' || c == '"') out << '\\';
                    if (c == '\n') out << "\\n";
                    else if (c == '\r') out << "\\r";
                    else if (c == '\t') out << "\\t";
                    else out << c;
                }
                out << '"';
            } else {
                out << array_string(data[i], memory);
            }
        }
        out << ']';
        return out.str();
    }

    static std::string value_string(const Value& value, const RuntimeMemory& memory) {
        if (value.tag == Value::Tag::Float) {
            std::ostringstream out;
            out << std::setprecision(15) << value.data.floating;
            return out.str();
        }

        uint64_t raw = value.data.nat;
        switch (heap_tag(raw)) {
            case HeapTag::None:
                return std::to_string(raw);
            case HeapTag::String:
                return std::string(memory.string_view(raw));
            case HeapTag::NatArray:
            case HeapTag::FloatArray:
            case HeapTag::StringArray:
            case HeapTag::ArrayArray:
                return array_string(raw, memory);
            default:
                throw std::runtime_error("unknown enclosed data type tag " + std::to_string(static_cast<unsigned>(heap_tag(raw))));
        }
    }

    struct ReuseHint {
        size_t argument_index = 0;
        uint64_t handle = 0;
    };

    struct BuiltinContext {
        std::vector<ReuseHint> releasable;
    };

    struct BuiltinResult {
        Value value;
        bool owns_handle = false;
        std::optional<size_t> transferred_argument;
    };

    static bool runtime_path_is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
        auto root_it = root.begin();
        auto candidate_it = candidate.begin();
        for (; root_it != root.end(); ++root_it, ++candidate_it) {
            if (candidate_it == candidate.end() || *root_it != *candidate_it) return false;
        }
        return true;
    }

    static const std::filesystem::path& runtime_workspace_root() {
        static const std::filesystem::path root = std::filesystem::weakly_canonical(std::filesystem::current_path());
        return root;
    }

    static std::filesystem::path runtime_workspace_path(std::string_view relative, bool require_exists) {
        if (relative.empty()) throw std::runtime_error("file path must not be empty");
        std::filesystem::path rel{std::string(relative)};
        if (rel.is_absolute()) throw std::runtime_error("file paths must be relative to the working directory");
        const std::filesystem::path& root = runtime_workspace_root();
        std::filesystem::path candidate = std::filesystem::weakly_canonical(root / rel);
        if (!runtime_path_is_within(root, candidate)) throw std::runtime_error("file path escapes the working directory");
        if (require_exists && !std::filesystem::exists(candidate)) {
            throw std::runtime_error("file does not exist: " + std::string(relative));
        }
        return candidate;
    }

    static std::filesystem::path runtime_load_path(std::string_view relative) {
        std::filesystem::path path = runtime_workspace_path(relative, true);
        if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("load path is not a regular file: " + std::string(relative));
        return path;
    }

    static std::filesystem::path runtime_save_path(std::string_view relative) {
        std::filesystem::path path = runtime_workspace_path(relative, false);
        if (std::filesystem::exists(path)) throw std::runtime_error("refusing to overwrite existing file: " + std::string(relative));
        const std::filesystem::path& root = runtime_workspace_root();
        std::filesystem::path parent = std::filesystem::weakly_canonical(path.parent_path());
        if (!runtime_path_is_within(root, parent) || !std::filesystem::is_directory(parent)) {
            throw std::runtime_error("save parent directory does not exist inside the working directory");
        }
        return path;
    }

    static std::string_view trim_ascii_space(std::string_view text) {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
        return text;
    }

    static std::string display_implementation_name(const Interner& names, NameId fn) {
        if (fn >= names.strings.size()) return "<invalid NameId " + std::to_string(fn) + ">";
        std::string name(names.strings[fn]);
        if (name.starts_with("Impl::")) name.erase(0, 6);
        return name;
    }

    static size_t fuzzy_name_distance(std::string_view left, std::string_view right) {
        std::vector<size_t> previous(right.size() + 1);
        std::vector<size_t> current(right.size() + 1);
        std::vector<size_t> before_previous(right.size() + 1);
        for (size_t j = 0; j <= right.size(); ++j) previous[j] = j;
        for (size_t i = 1; i <= left.size(); ++i) {
            current[0] = i;
            for (size_t j = 1; j <= right.size(); ++j) {
                size_t substitution = previous[j - 1] + (left[i - 1] == right[j - 1] ? 0 : 1);
                size_t insertion = current[j - 1] + 1;
                size_t deletion = previous[j] + 1;
                current[j] = std::min({substitution, insertion, deletion});
                if (i > 1 && j > 1 && left[i - 1] == right[j - 2] && left[i - 2] == right[j - 1]) {
                    current[j] = std::min(current[j], before_previous[j - 2] + 1);
                }
            }
            before_previous.swap(previous);
            previous.swap(current);
        }
        return previous[right.size()];
    }

    static constexpr std::array<NameId, 40> registered_builtin_ids = {
        N_Impl_ncast, N_Impl_fcast,
        N_Impl_nadd, N_Impl_nsub, N_Impl_nmul, N_Impl_ndiv, N_Impl_npow, N_Impl_nmod, N_Impl_nmin, N_Impl_nmax,
        N_Impl_fadd, N_Impl_fsub, N_Impl_fmul, N_Impl_fdiv, N_Impl_fpow, N_Impl_fmin, N_Impl_fmax, N_Impl_flog,
        N_Impl_scat, N_Impl_nget, N_Impl_fget, N_Impl_nslice, N_Impl_fslice, N_Impl_sslice,
        N_Impl_sarray, N_Impl_narray, N_Impl_farray, N_Impl_aarray, N_Impl_acat, N_Impl_map, N_Impl_reduce,
        N_Impl_loadsarray, N_Impl_loadstring, N_Impl_savesarray, N_Impl_savestring, N_Impl_ssplit,
        N_Impl_ston, N_Impl_stof, N_Impl_ntos, N_Impl_ftos
    };

    static std::optional<std::string> closest_builtin_name(const Interner& names, NameId fn) {
        if (fn >= names.strings.size()) return std::nullopt;
        std::string requested = display_implementation_name(names, fn);
        size_t best_distance = std::numeric_limits<size_t>::max();
        std::string best;
        for (NameId candidate : registered_builtin_ids) {
            std::string candidate_name = display_implementation_name(names, candidate);
            size_t distance = fuzzy_name_distance(requested, candidate_name);
            if (distance < best_distance) {
                best_distance = distance;
                best = std::move(candidate_name);
            }
        }
        size_t threshold = std::max<size_t>(1, std::min<size_t>(4, (requested.size() + 2) / 3));
        if (best_distance <= threshold) return best;
        return std::nullopt;
    }

    static std::runtime_error unknown_builtin_error(const Interner& names, NameId fn) {
        std::string requested = display_implementation_name(names, fn);
        std::string message = "no C++ implementation registered for '" + requested + "'";
        if (auto suggestion = closest_builtin_name(names, fn)) message += "; did you mean '" + *suggestion + "'?";
        return std::runtime_error(message);
    }

    static std::string_view builtin_short_name(NameId fn) {
        switch (fn) {
            case N_Impl_ncast: return "ncast";
            case N_Impl_fcast: return "fcast";
            case N_Impl_nadd: return "nadd";
            case N_Impl_nsub: return "nsub";
            case N_Impl_nmul: return "nmul";
            case N_Impl_ndiv: return "ndiv";
            case N_Impl_npow: return "npow";
            case N_Impl_nmod: return "nmod";
            case N_Impl_nmin: return "nmin";
            case N_Impl_nmax: return "nmax";
            case N_Impl_fadd: return "fadd";
            case N_Impl_fsub: return "fsub";
            case N_Impl_fmul: return "fmul";
            case N_Impl_fdiv: return "fdiv";
            case N_Impl_fpow: return "fpow";
            case N_Impl_fmin: return "fmin";
            case N_Impl_fmax: return "fmax";
            case N_Impl_flog: return "flog";
            case N_Impl_scat: return "scat";
            case N_Impl_nget: return "nget";
            case N_Impl_fget: return "fget";
            case N_Impl_nslice: return "nslice";
            case N_Impl_fslice: return "fslice";
            case N_Impl_sslice: return "sslice";
            case N_Impl_sarray: return "sarray";
            case N_Impl_narray: return "narray";
            case N_Impl_farray: return "farray";
            case N_Impl_aarray: return "aarray";
            case N_Impl_acat: return "acat";
            case N_Impl_map: return "map";
            case N_Impl_reduce: return "reduce";
            case N_Impl_loadsarray: return "loadsarray";
            case N_Impl_loadstring: return "loadstring";
            case N_Impl_savesarray: return "savesarray";
            case N_Impl_savestring: return "savestring";
            case N_Impl_ssplit: return "ssplit";
            case N_Impl_ston: return "ston";
            case N_Impl_stof: return "stof";
            case N_Impl_ntos: return "ntos";
            case N_Impl_ftos: return "ftos";
            default: return "builtin";
        }
    }

    static void require_arity(NameId fn, size_t count) {
        auto exactly = [&](size_t wanted) {
            if (count != wanted) {
                throw std::runtime_error(std::string(builtin_short_name(fn)) + " requires exactly " + std::to_string(wanted) + (wanted == 1 ? " argument" : " arguments"));
            }
        };
        auto at_least_one = [&]() {
            if (count == 0) throw std::runtime_error(std::string(builtin_short_name(fn)) + " requires at least one argument");
        };

        switch (fn) {
            case N_Impl_ncast:
            case N_Impl_fcast:
            case N_Impl_loadsarray:
            case N_Impl_loadstring:
            case N_Impl_ston:
            case N_Impl_stof:
            case N_Impl_ntos:
            case N_Impl_ftos:
            case N_Impl_flog:
                exactly(1);
                return;
            case N_Impl_nsub:
            case N_Impl_ndiv:
            case N_Impl_npow:
            case N_Impl_nmod:
            case N_Impl_fsub:
            case N_Impl_fdiv:
            case N_Impl_fpow:
            case N_Impl_nget:
            case N_Impl_fget:
            case N_Impl_reduce:
            case N_Impl_savesarray:
            case N_Impl_savestring:
            case N_Impl_ssplit:
                exactly(2);
                return;
            case N_Impl_nslice:
            case N_Impl_fslice:
            case N_Impl_sslice:
                exactly(3);
                return;
            case N_Impl_nadd:
            case N_Impl_nmul:
            case N_Impl_nmin:
            case N_Impl_nmax:
            case N_Impl_fadd:
            case N_Impl_fmul:
            case N_Impl_fmin:
            case N_Impl_fmax:
            case N_Impl_scat:
            case N_Impl_acat:
                at_least_one();
                return;
            default:
                return;
        }
    }

    static HeapTag map_expected_output_tag(NameId fn) {
        switch (fn) {
            case N_Impl_ncast:
            case N_Impl_nadd:
            case N_Impl_nsub:
            case N_Impl_nmul:
            case N_Impl_ndiv:
            case N_Impl_npow:
            case N_Impl_nmod:
            case N_Impl_nmin:
            case N_Impl_nmax:
            case N_Impl_ston:
            case N_Impl_savesarray:
            case N_Impl_savestring:
                return HeapTag::NatArray;
            case N_Impl_fcast:
            case N_Impl_fadd:
            case N_Impl_fsub:
            case N_Impl_fmul:
            case N_Impl_fdiv:
            case N_Impl_fpow:
            case N_Impl_fmin:
            case N_Impl_fmax:
            case N_Impl_flog:
            case N_Impl_fget:
            case N_Impl_stof:
                return HeapTag::FloatArray;
            case N_Impl_scat:
            case N_Impl_sslice:
            case N_Impl_loadstring:
            case N_Impl_ntos:
            case N_Impl_ftos:
                return HeapTag::StringArray;
            case N_Impl_nslice:
            case N_Impl_fslice:
            case N_Impl_sarray:
            case N_Impl_narray:
            case N_Impl_farray:
            case N_Impl_aarray:
            case N_Impl_acat:
            case N_Impl_map:
            case N_Impl_loadsarray:
            case N_Impl_ssplit:
                return HeapTag::ArrayArray;
            default:
                return HeapTag::None;
        }
    }

    static bool map_target_allowed(NameId fn) {
        return std::find(registered_builtin_ids.begin(), registered_builtin_ids.end(), fn) != registered_builtin_ids.end();
    }

    static std::optional<ReuseHint> best_reusable_array(const BuiltinContext* context, RuntimeMemory& memory) {
        if (!context) return std::nullopt;
        std::optional<ReuseHint> best;
        uint64_t best_estimate = 0;
        for (const ReuseHint& hint : context->releasable) {
            if (!is_array_tag(heap_tag(hint.handle))) continue;
            if (!memory.can_reuse_array(hint.handle)) continue;
            uint64_t estimate = heap_start(hint.handle) + heap_size(hint.handle);
            if (!best || estimate > best_estimate) {
                best = hint;
                best_estimate = estimate;
            }
        }
        return best;
    }

    static std::optional<ReuseHint> best_reusable_string(const BuiltinContext* context, RuntimeMemory& memory) {
        if (!context) return std::nullopt;
        std::optional<ReuseHint> best;
        uint64_t best_estimate = 0;
        for (const ReuseHint& hint : context->releasable) {
            if (heap_tag(hint.handle) != HeapTag::String) continue;
            if (!memory.can_reuse_string(hint.handle)) continue;
            uint64_t estimate = heap_start(hint.handle) + heap_size(hint.handle);
            if (!best || estimate > best_estimate) {
                best = hint;
                best_estimate = estimate;
            }
        }
        return best;
    }

    static BuiltinResult allocate_array_result(HeapTag tag, const std::vector<uint64_t>& values, RuntimeMemory& memory, const BuiltinContext* context) {
        if (auto reuse = best_reusable_array(context, memory)) {
            uint64_t handle = memory.replace_array(reuse->handle, tag, values);
            return {Value::nat(handle), true, reuse->argument_index};
        }
        return {Value::nat(memory.allocate_array(tag, values)), true, std::nullopt};
    }

    static BuiltinResult builtin_call(NameId fn, const std::vector<Value>& args, RuntimeMemory& memory, Interner& names, const BuiltinContext* context = nullptr);

    static BuiltinResult map_builtin(const std::vector<Value>& args, RuntimeMemory& memory, Interner& names, const BuiltinContext* context) {
        if (args.size() < 2) throw std::runtime_error("map requires a function name and at least one value/array argument");
        std::string target_text(memory.string_view(string_handle(args[0], memory)));
        NameId target = names.intern(target_text.find("::") == std::string::npos ? "Impl::" + target_text : target_text);
        if (!map_target_allowed(target)) {
            std::string message = "map cannot apply unsupported function '" + display_implementation_name(names, target) + "'";
            if (auto suggestion = closest_builtin_name(names, target)) message += "; did you mean '" + *suggestion + "'?";
            throw std::runtime_error(message);
        }

        const size_t parameter_count = args.size() - 1;
        require_arity(target, parameter_count);
        bool has_array = false;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i].tag == Value::Tag::Nat && is_array_tag(heap_tag(args[i].data.nat))) has_array = true;
        }
        if (!has_array) throw std::runtime_error("map requires at least one array argument");

        HeapTag output_tag = map_expected_output_tag(target);
        bool output_tag_known = output_tag != HeapTag::None;
        std::vector<uint64_t> raw_results;
        std::vector<uint64_t> temporary_owned_handles;
        std::vector<Value> current(parameter_count);

        auto result_array_tag = [&](const BuiltinResult& result) {
            if (result.value.tag == Value::Tag::Float) return HeapTag::FloatArray;
            uint64_t raw = result.value.data.nat;
            HeapTag tag = heap_tag(raw);
            if (tag == HeapTag::None) return HeapTag::NatArray;
            if (tag == HeapTag::String) return HeapTag::StringArray;
            if (is_array_tag(tag)) return HeapTag::ArrayArray;
            throw std::runtime_error("map target returned a value with an unknown enclosed type");
        };

        std::function<void(size_t)> visit = [&](size_t index) {
            if (index == parameter_count) {
                BuiltinResult result = builtin_call(target, current, memory, names, nullptr);
                HeapTag actual_tag = result_array_tag(result);
                if (!output_tag_known) {
                    output_tag = actual_tag;
                    output_tag_known = true;
                } else if (actual_tag != output_tag) {
                    throw std::runtime_error("map target '" + display_implementation_name(names, target) + "' returned inconsistent value kinds");
                }

                if (output_tag == HeapTag::FloatArray) {
                    raw_results.push_back(std::bit_cast<uint64_t>(float_number(result.value)));
                } else {
                    if (result.value.tag != Value::Tag::Nat) throw std::runtime_error("map target returned an unexpected float value");
                    raw_results.push_back(result.value.data.nat);
                    if (result.owns_handle && value_is_handle(result.value)) temporary_owned_handles.push_back(result.value.data.nat);
                }
                if (raw_results.size() > HANDLE_SIZE_MASK) throw std::runtime_error("map result exceeds the 20-bit array size field");
                return;
            }

            const Value& input = args[index + 1];
            if (input.tag == Value::Tag::Nat && is_array_tag(heap_tag(input.data.nat))) {
                uint64_t handle = input.data.nat;
                const uint64_t* data = memory.array_data(handle);
                HeapTag tag = heap_tag(handle);
                for (uint64_t i = 0; i < heap_size(handle); ++i) {
                    if (tag == HeapTag::FloatArray) current[index] = Value::floating(std::bit_cast<double>(data[i]));
                    else current[index] = Value::nat(data[i]);
                    visit(index + 1);
                }
                return;
            }

            current[index] = input;
            visit(index + 1);
        };

        auto release_temporaries = [&]() {
            while (!temporary_owned_handles.empty()) {
                uint64_t handle = temporary_owned_handles.back();
                temporary_owned_handles.pop_back();
                memory.release(handle);
            }
        };
        try {
            visit(0);
            if (!output_tag_known) {
                throw std::runtime_error("map cannot infer the result array type because no combinations were produced");
            }
            BuiltinResult output = allocate_array_result(output_tag, raw_results, memory, context);
            release_temporaries();
            return output;
        } catch (...) {
            try { release_temporaries(); } catch (...) {}
            throw;
        }
    }

    static BuiltinResult reduce_builtin(const std::vector<Value>& args, RuntimeMemory& memory, Interner& names, const BuiltinContext* context) {
        std::string target_text(memory.string_view(string_handle(args[0], memory)));
        NameId target = names.intern(target_text.find("::") == std::string::npos ? "Impl::" + target_text : target_text);
        uint64_t container = array_handle(args[1], memory);
        const uint64_t* data = memory.array_data(container);
        uint64_t size = heap_size(container);
        HeapTag tag = heap_tag(container);

        auto require_nonempty = [&]() {
            if (size == 0) throw std::runtime_error("reduce '" + display_implementation_name(names, target) + "' requires a non-empty array");
        };

        switch (target) {
            case N_Impl_nadd:
            case N_Impl_nmul:
            case N_Impl_nmin:
            case N_Impl_nmax: {
                if (tag != HeapTag::NatArray) throw std::runtime_error("natural reduce requires an narray");
                require_nonempty();
                if (heap_tag(data[0]) != HeapTag::None) throw std::runtime_error("natural reduce encountered an enclosed handle instead of a plain natural");
                uint64_t result = target == N_Impl_nmul ? 1 : data[0];
                uint64_t begin = target == N_Impl_nmul ? 0 : 1;
                for (uint64_t i = begin; i < size; ++i) {
                    if (heap_tag(data[i]) != HeapTag::None) throw std::runtime_error("natural reduce encountered an enclosed handle instead of a plain natural");
                    if (target == N_Impl_nadd) result = checked_nat_add(result, data[i]);
                    else if (target == N_Impl_nmul) result = checked_nat_mul(result, data[i]);
                    else if (target == N_Impl_nmin) result = std::min(result, data[i]);
                    else result = std::max(result, data[i]);
                }
                return {Value::nat(result), false, std::nullopt};
            }
            case N_Impl_fadd:
            case N_Impl_fmul:
            case N_Impl_fmin:
            case N_Impl_fmax: {
                if (tag != HeapTag::FloatArray) throw std::runtime_error("float reduce requires an farray");
                require_nonempty();
                double result = target == N_Impl_fmul ? 1.0 : std::bit_cast<double>(data[0]);
                uint64_t begin = target == N_Impl_fmul ? 0 : 1;
                for (uint64_t i = begin; i < size; ++i) {
                    double value = std::bit_cast<double>(data[i]);
                    if (target == N_Impl_fadd) result += value;
                    else if (target == N_Impl_fmul) result *= value;
                    else if (target == N_Impl_fmin) result = std::min(result, value);
                    else result = std::max(result, value);
                }
                return {Value::floating(result), false, std::nullopt};
            }
            case N_Impl_scat: {
                if (tag != HeapTag::StringArray) throw std::runtime_error("reduce 'scat' requires an sarray");
                std::string joined;
                size_t total = 0;
                for (uint64_t i = 0; i < size; ++i) total += memory.string_view(data[i]).size();
                if (total > HANDLE_SIZE_MASK) throw std::runtime_error("scat result exceeds the 20-bit string size field");
                joined.reserve(total);
                for (uint64_t i = 0; i < size; ++i) joined += memory.string_view(data[i]);
                if (auto reuse = best_reusable_string(context, memory)) {
                    uint64_t handle = memory.replace_string(reuse->handle, std::move(joined));
                    return {Value::nat(handle), true, reuse->argument_index};
                }
                return {Value::nat(memory.allocate_string(std::move(joined))), true, std::nullopt};
            }
            case N_Impl_acat: {
                if (tag != HeapTag::ArrayArray) throw std::runtime_error("reduce 'acat' requires an aarray");
                require_nonempty();
                std::vector<uint64_t> flat;
                uint64_t total = 0;
                HeapTag nested_tag = heap_tag(data[0]);
                if (!is_array_tag(nested_tag)) throw std::runtime_error("reduce 'acat' encountered a non-array element");
                for (uint64_t i = 0; i < size; ++i) {
                    memory.checked_array_meta(data[i]);
                    if (heap_tag(data[i]) != nested_tag) throw std::runtime_error("reduce 'acat' requires all nested arrays to have the same view type");
                    if (heap_size(data[i]) > HANDLE_SIZE_MASK - total) throw std::runtime_error("acat result exceeds the 20-bit array size field");
                    total += heap_size(data[i]);
                }
                flat.reserve(static_cast<size_t>(total));
                for (uint64_t i = 0; i < size; ++i) {
                    const uint64_t* nested = memory.array_data(data[i]);
                    flat.insert(flat.end(), nested, nested + heap_size(data[i]));
                }
                return allocate_array_result(nested_tag, flat, memory, context);
            }
            default: {
                std::string message = "reduce cannot apply unsupported function '" + display_implementation_name(names, target) + "'";
                if (auto suggestion = closest_builtin_name(names, target)) message += "; did you mean '" + *suggestion + "'?";
                throw std::runtime_error(message);
            }
        }
    }

    static BuiltinResult builtin_call(NameId fn, const std::vector<Value>& args, RuntimeMemory& memory, Interner& names, const BuiltinContext* context) {
        require_arity(fn, args.size());

        switch (fn) {
            case N_Impl_ncast: {
                if (args[0].tag == Value::Tag::Nat) {
                    if (heap_tag(args[0].data.nat) != HeapTag::None) throw std::runtime_error("ncast cannot cast an enclosed handle to a plain natural");
                    return {args[0], false, std::nullopt};
                }
                double value = float_number(args[0]);
                if (!std::isfinite(value) || value < 0.0 || value > static_cast<double>(MAX_PLAIN_NAT)) {
                    throw std::runtime_error("ncast cannot represent this float as a tag-0 natural");
                }
                return {Value::nat(static_cast<uint64_t>(value)), false, std::nullopt};
            }
            case N_Impl_fcast:
                if (args[0].tag == Value::Tag::Float) return {args[0], false, std::nullopt};
                return {Value::floating(static_cast<double>(plain_nat(args[0]))), false, std::nullopt};
            case N_Impl_nadd: {
                uint64_t result = 0;
                for (const Value& value : args) result = checked_nat_add(result, plain_nat(value));
                return {Value::nat(result), false, std::nullopt};
            }
            case N_Impl_nsub: {
                uint64_t left = plain_nat(args[0]);
                uint64_t right = plain_nat(args[1]);
                if (right > left) throw std::runtime_error("natural subtraction would underflow");
                return {Value::nat(left - right), false, std::nullopt};
            }
            case N_Impl_nmul: {
                uint64_t result = 1;
                for (const Value& value : args) result = checked_nat_mul(result, plain_nat(value));
                return {Value::nat(result), false, std::nullopt};
            }
            case N_Impl_ndiv: {
                uint64_t divisor = plain_nat(args[1]);
                if (divisor == 0) throw std::runtime_error("natural division by zero");
                return {Value::nat(plain_nat(args[0]) / divisor), false, std::nullopt};
            }
            case N_Impl_npow:
                return {Value::nat(checked_nat_pow(plain_nat(args[0]), plain_nat(args[1]))), false, std::nullopt};
            case N_Impl_nmod: {
                uint64_t divisor = plain_nat(args[1]);
                if (divisor == 0) throw std::runtime_error("natural modulo by zero");
                return {Value::nat(plain_nat(args[0]) % divisor), false, std::nullopt};
            }
            case N_Impl_nmin: {
                uint64_t result = plain_nat(args[0]);
                for (size_t i = 1; i < args.size(); ++i) result = std::min(result, plain_nat(args[i]));
                return {Value::nat(result), false, std::nullopt};
            }
            case N_Impl_nmax: {
                uint64_t result = plain_nat(args[0]);
                for (size_t i = 1; i < args.size(); ++i) result = std::max(result, plain_nat(args[i]));
                return {Value::nat(result), false, std::nullopt};
            }
            case N_Impl_fadd: {
                double result = 0.0;
                for (const Value& value : args) result += float_number(value);
                return {Value::floating(result), false, std::nullopt};
            }
            case N_Impl_fsub:
                return {Value::floating(float_number(args[0]) - float_number(args[1])), false, std::nullopt};
            case N_Impl_fmul: {
                double result = 1.0;
                for (const Value& value : args) result *= float_number(value);
                return {Value::floating(result), false, std::nullopt};
            }
            case N_Impl_fdiv: {
                double divisor = float_number(args[1]);
                if (divisor == 0.0) throw std::runtime_error("float division by zero");
                return {Value::floating(float_number(args[0]) / divisor), false, std::nullopt};
            }
            case N_Impl_fpow:
                return {Value::floating(std::pow(float_number(args[0]), float_number(args[1]))), false, std::nullopt};
            case N_Impl_fmin: {
                double result = float_number(args[0]);
                for (size_t i = 1; i < args.size(); ++i) result = std::min(result, float_number(args[i]));
                return {Value::floating(result), false, std::nullopt};
            }
            case N_Impl_fmax: {
                double result = float_number(args[0]);
                for (size_t i = 1; i < args.size(); ++i) result = std::max(result, float_number(args[i]));
                return {Value::floating(result), false, std::nullopt};
            }
            case N_Impl_flog:
                return {Value::floating(std::log(float_number(args[0]))), false, std::nullopt};
            case N_Impl_scat: {
                std::string result;
                size_t total = 0;
                for (const Value& value : args) total += memory.string_view(string_handle(value, memory)).size();
                if (total > HANDLE_SIZE_MASK) throw std::runtime_error("scat result exceeds the 20-bit string size field");
                result.reserve(total);
                for (const Value& value : args) result += memory.string_view(string_handle(value, memory));
                if (auto reuse = best_reusable_string(context, memory)) {
                    uint64_t handle = memory.replace_string(reuse->handle, std::move(result));
                    return {Value::nat(handle), true, reuse->argument_index};
                }
                return {Value::nat(memory.allocate_string(std::move(result))), true, std::nullopt};
            }
            case N_Impl_loadstring: {
                std::string path_text(memory.string_view(string_handle(args[0], memory)));
                std::string contents = read_file(runtime_load_path(path_text));
                if (contents.size() > HANDLE_SIZE_MASK) throw std::runtime_error("loadstring file is too large for a 20-bit string handle");
                return {Value::nat(memory.allocate_string(std::move(contents))), true, std::nullopt};
            }
            case N_Impl_loadsarray: {
                std::string path_text(memory.string_view(string_handle(args[0], memory)));
                std::string contents = read_file(runtime_load_path(path_text));
                std::vector<uint64_t> lines;
                size_t position = 0;
                try {
                    while (position < contents.size()) {
                        size_t newline = contents.find('\n', position);
                        size_t end = newline == std::string::npos ? contents.size() : newline;
                        size_t line_end = end;
                        if (line_end > position && contents[line_end - 1] == '\r') --line_end;
                        lines.push_back(memory.allocate_string(contents.substr(position, line_end - position)));
                        if (lines.size() > HANDLE_SIZE_MASK) throw std::runtime_error("loadsarray file has too many lines for a 20-bit array handle");
                        if (newline == std::string::npos) break;
                        position = newline + 1;
                    }
                    uint64_t result = memory.allocate_array(HeapTag::StringArray, lines);
                    while (!lines.empty()) {
                        uint64_t line = lines.back();
                        lines.pop_back();
                        memory.release(line);
                    }
                    return {Value::nat(result), true, std::nullopt};
                } catch (...) {
                    while (!lines.empty()) {
                        uint64_t line = lines.back();
                        lines.pop_back();
                        try { memory.release(line); } catch (...) {}
                    }
                    throw;
                }
            }
            case N_Impl_savestring: {
                std::string path_text(memory.string_view(string_handle(args[0], memory)));
                std::string_view contents = memory.string_view(string_handle(args[1], memory));
                std::filesystem::path path = runtime_save_path(path_text);
                std::ofstream output(path, std::ios::binary);
                if (!output) throw std::runtime_error("cannot open file for writing: " + path_text);
                output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
                if (!output) throw std::runtime_error("failed while writing file: " + path_text);
                return {Value::nat(static_cast<uint64_t>(contents.size())), false, std::nullopt};
            }
            case N_Impl_savesarray: {
                std::string path_text(memory.string_view(string_handle(args[0], memory)));
                uint64_t array = array_handle(args[1], memory);
                memory.checked_array_meta(array, HeapTag::StringArray);
                const uint64_t* lines = memory.array_data(array, HeapTag::StringArray);
                uint64_t count = heap_size(array);
                uint64_t byte_count = count == 0 ? 0 : count - 1;
                for (uint64_t i = 0; i < count; ++i) {
                    size_t size = memory.string_view(lines[i]).size();
                    if (size > MAX_PLAIN_NAT - byte_count) throw std::runtime_error("savesarray output is too large");
                    byte_count += size;
                }
                std::filesystem::path path = runtime_save_path(path_text);
                std::ofstream output(path, std::ios::binary);
                if (!output) throw std::runtime_error("cannot open file for writing: " + path_text);
                for (uint64_t i = 0; i < count; ++i) {
                    if (i) output.put('\n');
                    std::string_view line = memory.string_view(lines[i]);
                    output.write(line.data(), static_cast<std::streamsize>(line.size()));
                }
                if (!output) throw std::runtime_error("failed while writing file: " + path_text);
                return {Value::nat(byte_count), false, std::nullopt};
            }
            case N_Impl_ssplit: {
                uint64_t source = string_handle(args[0], memory);
                std::string_view text = memory.string_view(source);
                std::string_view separator = memory.string_view(string_handle(args[1], memory));
                if (separator.empty()) throw std::runtime_error("ssplit separator must not be empty");
                std::vector<uint64_t> parts;
                size_t position = 0;
                try {
                    while (true) {
                        size_t found = text.find(separator, position);
                        size_t end = found == std::string_view::npos ? text.size() : found;
                        parts.push_back(memory.slice_string(source, position, end - position));
                        if (parts.size() > HANDLE_SIZE_MASK) throw std::runtime_error("ssplit result exceeds the 20-bit array size field");
                        if (found == std::string_view::npos) break;
                        position = found + separator.size();
                    }
                    uint64_t result = memory.allocate_array(HeapTag::StringArray, parts);
                    while (!parts.empty()) {
                        uint64_t part = parts.back();
                        parts.pop_back();
                        memory.release(part);
                    }
                    return {Value::nat(result), true, std::nullopt};
                } catch (...) {
                    while (!parts.empty()) {
                        uint64_t part = parts.back();
                        parts.pop_back();
                        try { memory.release(part); } catch (...) {}
                    }
                    throw;
                }
            }
            case N_Impl_ston: {
                std::string_view text = trim_ascii_space(memory.string_view(string_handle(args[0], memory)));
                if (text.empty()) throw std::runtime_error("ston cannot convert an empty string to nat");
                uint64_t value = 0;
                auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 10);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value > MAX_PLAIN_NAT) {
                    throw std::runtime_error("ston cannot convert '" + std::string(text) + "' to nat");
                }
                return {Value::nat(value), false, std::nullopt};
            }
            case N_Impl_stof: {
                std::string_view text = trim_ascii_space(memory.string_view(string_handle(args[0], memory)));
                double value = std::numeric_limits<double>::quiet_NaN();
                if (!text.empty()) {
                    double parsed_value = 0.0;
                    auto parsed = std::from_chars(text.data(), text.data() + text.size(), parsed_value, std::chars_format::general);
                    if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) value = parsed_value;
                }
                return {Value::floating(value), false, std::nullopt};
            }
            case N_Impl_ntos:
                return {Value::nat(memory.allocate_string(std::to_string(plain_nat(args[0])))), true, std::nullopt};
            case N_Impl_ftos: {
                std::ostringstream out;
                out << std::setprecision(17) << float_number(args[0]);
                return {Value::nat(memory.allocate_string(out.str())), true, std::nullopt};
            }
            case N_Impl_nget: {
                uint64_t index = plain_nat(args[1]);
                if (args[0].tag != Value::Tag::Nat) throw std::runtime_error("nget requires a string or array as its first argument");
                uint64_t handle = args[0].data.nat;
                if (heap_tag(handle) == HeapTag::String) {
                    std::string_view view = memory.string_view(handle);
                    if (index >= view.size()) throw std::runtime_error("nget index " + std::to_string(index) + " is out of bounds for string size " + std::to_string(view.size()));
                    return {Value::nat(static_cast<unsigned char>(view[static_cast<size_t>(index)])), false, std::nullopt};
                }
                if (!is_array_tag(heap_tag(handle))) throw std::runtime_error("nget requires a string or array as its first argument");
                memory.checked_array_meta(handle);
                if (index >= heap_size(handle)) throw std::runtime_error("nget index " + std::to_string(index) + " is out of bounds for array size " + std::to_string(heap_size(handle)));
                uint64_t raw = memory.array_data(handle)[index];
                if ((heap_tag(handle) == HeapTag::StringArray || heap_tag(handle) == HeapTag::ArrayArray) && RuntimeMemory::is_handle(raw)) {
                    memory.retain(raw);
                    return {Value::nat(raw), true, std::nullopt};
                }
                return {Value::nat(raw), false, std::nullopt};
            }
            case N_Impl_fget: {
                uint64_t handle = array_handle(args[0], memory);
                uint64_t index = plain_nat(args[1]);
                if (index >= heap_size(handle)) throw std::runtime_error("fget index " + std::to_string(index) + " is out of bounds for array size " + std::to_string(heap_size(handle)));
                return {Value::floating(std::bit_cast<double>(memory.array_data(handle)[index])), false, std::nullopt};
            }
            case N_Impl_nslice: {
                uint64_t handle = array_handle(args[0], memory);
                uint64_t start = plain_nat(args[1]);
                uint64_t size = plain_nat(args[2]);
                return {Value::nat(memory.slice_array_as(handle, HeapTag::NatArray, start, size)), true, std::nullopt};
            }
            case N_Impl_fslice: {
                uint64_t handle = array_handle(args[0], memory);
                uint64_t start = plain_nat(args[1]);
                uint64_t size = plain_nat(args[2]);
                return {Value::nat(memory.slice_array_as(handle, HeapTag::FloatArray, start, size)), true, std::nullopt};
            }
            case N_Impl_sslice: {
                uint64_t handle = string_handle(args[0], memory);
                uint64_t start = plain_nat(args[1]);
                uint64_t size = plain_nat(args[2]);
                return {Value::nat(memory.slice_string(handle, start, size)), true, std::nullopt};
            }
            case N_Impl_narray:
            case N_Impl_farray:
            case N_Impl_sarray: {
                HeapTag wanted = fn == N_Impl_narray ? HeapTag::NatArray : fn == N_Impl_farray ? HeapTag::FloatArray : HeapTag::StringArray;
                if (args.size() == 1 && args[0].tag == Value::Tag::Nat && is_array_tag(heap_tag(args[0].data.nat))) {
                    return {Value::nat(memory.retain_array_as(args[0].data.nat, wanted)), true, std::nullopt};
                }

                std::vector<uint64_t> flat;
                for (const Value& value : args) {
                    if (value.tag == Value::Tag::Nat && is_array_tag(heap_tag(value.data.nat))) {
                        uint64_t handle = array_handle(value, memory);
                        const uint64_t* data = memory.array_data(handle);
                        if (heap_size(handle) > HANDLE_SIZE_MASK - flat.size()) throw std::runtime_error("array result exceeds the 20-bit size field");
                        flat.insert(flat.end(), data, data + heap_size(handle));
                        continue;
                    }
                    if (wanted == HeapTag::NatArray) {
                        if (value.tag != Value::Tag::Nat) throw std::runtime_error("narray scalar elements must be natural/raw-word values");
                        flat.push_back(value.data.nat);
                    } else if (wanted == HeapTag::FloatArray) {
                        flat.push_back(std::bit_cast<uint64_t>(float_number(value)));
                    } else {
                        flat.push_back(string_handle(value, memory));
                    }
                    if (flat.size() > HANDLE_SIZE_MASK) throw std::runtime_error("array result exceeds the 20-bit size field");
                }
                return allocate_array_result(wanted, flat, memory, context);
            }
            case N_Impl_aarray: {
                if (args.size() == 1 && args[0].tag == Value::Tag::Nat && heap_tag(args[0].data.nat) == HeapTag::ArrayArray) {
                    return {Value::nat(memory.retain_array_as(args[0].data.nat, HeapTag::ArrayArray)), true, std::nullopt};
                }
                std::vector<uint64_t> nested;
                for (const Value& value : args) {
                    if (value.tag != Value::Tag::Nat || !is_array_tag(heap_tag(value.data.nat))) throw std::runtime_error("aarray elements must be array handles");
                    uint64_t handle = value.data.nat;
                    if (heap_tag(handle) == HeapTag::ArrayArray) {
                        memory.checked_array_meta(handle, HeapTag::ArrayArray);
                        const uint64_t* data = memory.array_data(handle, HeapTag::ArrayArray);
                        nested.insert(nested.end(), data, data + heap_size(handle));
                    } else {
                        memory.checked_array_meta(handle);
                        nested.push_back(handle);
                    }
                    if (nested.size() > HANDLE_SIZE_MASK) throw std::runtime_error("aarray result exceeds the 20-bit size field");
                }
                return {Value::nat(memory.allocate_array(HeapTag::ArrayArray, nested)), true, std::nullopt};
            }
            case N_Impl_acat: {
                HeapTag output_tag = HeapTag::None;
                uint64_t total = 0;
                for (const Value& value : args) {
                    uint64_t handle = array_handle(value, memory);
                    if (output_tag == HeapTag::None) output_tag = heap_tag(handle);
                    if (heap_tag(handle) != output_tag) throw std::runtime_error("acat requires arrays with the same view type");
                    if (heap_size(handle) > HANDLE_SIZE_MASK - total) throw std::runtime_error("acat result exceeds the 20-bit array size field");
                    total += heap_size(handle);
                }
                std::vector<uint64_t> flat;
                flat.reserve(static_cast<size_t>(total));
                for (const Value& value : args) {
                    uint64_t handle = value.data.nat;
                    const uint64_t* data = memory.array_data(handle);
                    flat.insert(flat.end(), data, data + heap_size(handle));
                }
                return allocate_array_result(output_tag, flat, memory, context);
            }
            case N_Impl_map:
                return map_builtin(args, memory, names, context);
            case N_Impl_reduce:
                return reduce_builtin(args, memory, names, context);
            default:
                throw unknown_builtin_error(names, fn);
        }
    }

    struct Routes {
        std::unordered_map<NodeId, std::vector<Edge>> args;
        std::unordered_map<NodeId, Edge> rets;
    };

    static bool is_arg_tag(const Interner& names, NameId tag) {
        std::string_view s = names.str(tag);
        if (s == "arg") return true;
        if (!s.starts_with("arg") || s.size() == 3) return false;
        return std::all_of(s.begin() + 3, s.end(), [](char c) {
            return std::isdigit(static_cast<unsigned char>(c));
        });
    }

    static Routes call_routes(const Graph& g) {
        Routes routes;
        NameId returns = g.names->intern("returns");
        for (const Edge& edge : g.edges) {
            std::string_view tag = g.names->str(edge.tag);
            if (tag.starts_with("!")) continue;
            if (is_arg_tag(*g.names, edge.tag)) {
                routes.args[edge.right].push_back(edge);
            } else if (edge.tag == returns) {
                if (routes.rets.count(edge.left)) throw std::runtime_error("call '" + best_name(g, edge.left) + "' has multiple returns routes");
                routes.rets[edge.left] = edge;
            }
        }
        return routes;
    }

    static std::vector<NodeId> runtime_roots(const Graph& g) {
        Routes routes = call_routes(g);
        std::set<NodeId> calls;
        std::set<NodeId> produced;
        for (const auto& [id, edges] : routes.args) {
            (void)edges;
            calls.insert(id);
        }
        for (const auto& [id, edge] : routes.rets) {
            calls.insert(id);
            produced.insert(edge.right);
        }
        std::vector<NodeId> out;
        for (NodeId id : g.ids()) {
            if (!calls.count(id) && !produced.count(id)) out.push_back(id);
        }
        return out;
    }

    static std::vector<NodeId> runtime_sinks(const Graph& g) {
        Routes routes = call_routes(g);
        std::set<NodeId> used;
        std::set<NodeId> produced;
        for (const auto& [id, edges] : routes.args) {
            (void)id;
            for (const Edge& edge : edges) used.insert(edge.left);
        }
        for (const auto& [id, edge] : routes.rets) {
            (void)id;
            produced.insert(edge.right);
        }
        std::vector<NodeId> out;
        for (NodeId id : produced) {
            if (!used.count(id)) out.push_back(id);
        }
        if (!out.empty()) return out;
        for (NodeId id : runtime_roots(g)) {
            if (!used.count(id)) out.push_back(id);
        }
        return out;
    }

    enum class Converter : uint8_t {
        Nat,
        Float,
        String
    };

    static std::optional<std::pair<NameId, Converter>> primitive_converter(const Graph& g, NodeId id) {
        std::vector<std::pair<NameId, Converter>> converters;
        for (NameId type : g.types->names_for(g.nodes[id].types)) {
            switch (type) {
                case N_Impl_Nat:
                case N_Impl_Int:
                case N_Impl_nat:
                case N_Impl_int:
                    converters.push_back({type, Converter::Nat});
                    break;
                case N_Impl_Real:
                case N_Impl_Float:
                case N_Impl_real:
                case N_Impl_float:
                    converters.push_back({type, Converter::Float});
                    break;
                case N_Impl_String:
                case N_Impl_string:
                    converters.push_back({type, Converter::String});
                    break;
                default:
                    break;
            }
        }
        if (converters.size() != 1) return std::nullopt;
        return converters[0];
    }

    static bool literal_matches_converter(std::string_view raw, Converter converter) {
        raw = literal_text(raw);
        if (converter == Converter::String) return is_string_literal(raw);
        if (!is_numeric_literal(raw)) return false;
        bool is_float = raw.find('.') != std::string_view::npos;
        return converter == Converter::Float ? is_float : !is_float;
    }

    static std::optional<std::string_view> node_default_literal_text(const Graph& g, NodeId id) {
        auto primitive = primitive_converter(g, id);
        if (!primitive) return std::nullopt;
        Converter converter = primitive->second;
        std::vector<NameId> literals;
        for (const Alias& alias : g.nodes[id].names) {
            std::string_view alias_text = g.names->str(alias.name);
            if (!literal_matches_converter(alias_text, converter)) continue;
            std::string_view text = literal_text(alias_text);
            bool duplicate = false;
            for (NameId existing : literals) {
                if (literal_text(g.names->str(existing)) == text) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) literals.push_back(alias.name);
        }
        if (literals.size() != 1) return std::nullopt;
        return literal_text(g.names->str(literals[0]));
    }

    static bool node_has_default_literal(const Graph& g, NodeId id) {
        return node_default_literal_text(g, id).has_value();
    }

    static std::optional<Value> node_literal_value(const Graph& g, NodeId id, RuntimeMemory& memory) {
        auto primitive = primitive_converter(g, id);
        if (!primitive) return std::nullopt;
        auto raw_opt = node_default_literal_text(g, id);
        if (!raw_opt) return std::nullopt;
        std::string_view raw = *raw_opt;
        switch (primitive->second) {
            case Converter::String:
                return Value::nat(memory.allocate_string(unquote_string(raw)));
            case Converter::Nat: {
                uint64_t value = 0;
                auto [ptr, error] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
                if (error != std::errc{} || ptr != raw.data() + raw.size()) return std::nullopt;
                if (value > MAX_PLAIN_NAT) throw std::runtime_error("natural literal exceeds the tag-0 60-bit range");
                return Value::nat(value);
            }
            case Converter::Float: {
                std::string text(raw);
                char* end = nullptr;
                double value = std::strtod(text.c_str(), &end);
                if (end != text.c_str() + text.size()) return std::nullopt;
                return Value::floating(value);
            }
        }
        return std::nullopt;
    }

    static std::pair<NameId, Converter> root_converter(const Graph& g, NodeId id) {
        auto converter = primitive_converter(g, id);
        if (!converter) {
            throw std::runtime_error("root '" + best_name(g, id) + "' needs exactly one primitive converter; available types are " + g.types->format(g.nodes[id].types));
        }
        return *converter;
    }

    static std::string trim_ascii(std::string_view raw) {
        size_t begin = 0;
        size_t end = raw.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin]))) ++begin;
        while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1]))) --end;
        return std::string(raw.substr(begin, end - begin));
    }

    static Value convert_input(std::string_view raw, Converter converter, RuntimeMemory& memory) {
        if (converter == Converter::String) return Value::nat(memory.allocate_string(std::string(raw)));

        std::string text = trim_ascii(raw);
        if (converter == Converter::Nat) {
            uint64_t value = 0;
            auto [ptr, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || ptr != text.data() + text.size()) throw std::runtime_error("not a natural number");
            if (value > MAX_PLAIN_NAT) throw std::runtime_error("natural input exceeds the tag-0 60-bit range");
            return Value::nat(value);
        }

        char* end = nullptr;
        double value = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size()) throw std::runtime_error("not a float");
        return Value::floating(value);
    }

    struct ExecResult {
        std::unordered_map<NodeId, Value> values;
        std::vector<NodeId> sinks;
        std::shared_ptr<RuntimeMemory> memory;
    };

    static void release_node_memory(Graph& graph, NodeId id, std::unordered_map<NodeId, Value>& values, RuntimeMemory& memory, const std::set<NodeId>& sinks) {
        Node& node = graph.nodes[id];
        if (sinks.count(id)) return;
        if (node.used_outgoing_edges < node.outgoing_edges) return;
        auto it = values.find(id);
        if (it == values.end() || !value_is_handle(it->second) || node.memory == nullptr) return;
        memory.release(it->second.data.nat);
        node.memory = nullptr;
    }

    static ExecResult execute_graph(Graph& graph, const std::unordered_map<NameId, std::string>& raw) {
        auto memory = std::make_shared<RuntimeMemory>();
        std::unordered_map<NodeId, Value> values;
        Routes routes = call_routes(graph);
        std::vector<NodeId> sinks_vector = runtime_sinks(graph);
        std::set<NodeId> sinks(sinks_vector.begin(), sinks_vector.end());

        for (NodeId id : graph.ids()) {
            graph.nodes[id].memory = nullptr;
            graph.nodes[id].outgoing_edges = 0;
            graph.nodes[id].used_outgoing_edges = 0;
        }
        for (const auto& [call, edges] : routes.args) {
            (void)call;
            for (const Edge& edge : edges) ++graph.nodes[edge.left].outgoing_edges;
        }

        std::vector<NodeId> roots = runtime_roots(graph);
        std::set<NodeId> root_set(roots.begin(), roots.end());
        auto graph_name_map = graph_names(graph, root_set);
        for (NodeId id : roots) {
            if (auto literal = node_literal_value(graph, id, *memory)) {
                values.emplace(id, *literal);
                if (value_is_handle(*literal)) graph.nodes[id].memory = memory->pointer_for(literal->data.nat);
                release_node_memory(graph, id, values, *memory, sinks);
                continue;
            }

            auto [type, converter] = root_converter(graph, id);
            NameId key = graph_name_map[id];
            auto input = raw.find(key);
            if (input == raw.end()) {
                throw std::runtime_error("missing input for " + std::string(graph.names->str(key)) + " (" + std::string(graph.names->str(type)) + ")");
            }
            try {
                Value value = convert_input(input->second, converter, *memory);
                values.emplace(id, value);
                if (value_is_handle(value)) graph.nodes[id].memory = memory->pointer_for(value.data.nat);
                release_node_memory(graph, id, values, *memory, sinks);
            } catch (const std::exception& error) {
                throw std::runtime_error("cannot convert input " + std::string(graph.names->str(key)) + "='" + input->second + "' as " + std::string(graph.names->str(type)) + ": " + error.what());
            }
        }

        std::set<NodeId> pending;
        for (const auto& [id, edges] : routes.args) {
            (void)edges;
            pending.insert(id);
        }
        for (const auto& [id, edge] : routes.rets) {
            (void)edge;
            pending.insert(id);
        }

        while (!pending.empty()) {
            bool progress = false;
            for (auto it = pending.begin(); it != pending.end();) {
                NodeId call = *it;
                if (!routes.args.count(call) || !routes.rets.count(call)) {
                    throw std::runtime_error("call '" + best_name(graph, call) + "' requires arg/argN routes and exactly one returns route");
                }

                std::vector<Edge> inputs = routes.args[call];
                bool ready = true;
                for (const Edge& edge : inputs) {
                    if (!values.count(edge.left)) {
                        ready = false;
                        break;
                    }
                }
                if (!ready) {
                    ++it;
                    continue;
                }

                std::vector<Edge> ordered;
                bool all_variadic = std::all_of(inputs.begin(), inputs.end(), [&](const Edge& edge) {
                    return graph.names->str(edge.tag) == "arg";
                });
                if (all_variadic) {
                    ordered = inputs;
                } else {
                    std::vector<std::pair<int, Edge>> numbered;
                    for (const Edge& edge : inputs) {
                        std::string tag(graph.names->str(edge.tag));
                        if (!tag.starts_with("arg") || tag.size() == 3 || !std::all_of(tag.begin() + 3, tag.end(), [](char c) {
                            return std::isdigit(static_cast<unsigned char>(c));
                        })) {
                            throw std::runtime_error("call '" + best_name(graph, call) + "' cannot mix variadic arg routes with positional argN routes");
                        }
                        numbered.push_back({std::stoi(tag.substr(3)), edge});
                    }
                    std::sort(numbered.begin(), numbered.end(), [](const auto& left, const auto& right) {
                        return left.first < right.first;
                    });
                    for (size_t index = 0; index < numbered.size(); ++index) {
                        if (numbered[index].first != static_cast<int>(index)) {
                            throw std::runtime_error("call '" + best_name(graph, call) + "' requires contiguous argN routes starting at arg0");
                        }
                        ordered.push_back(numbered[index].second);
                    }
                }

                auto function = call_name_id(graph, call);
                if (!function) throw std::runtime_error("call '" + best_name(graph, call) + "' needs exactly one string literal implementation type");

                std::vector<Value> arguments;
                arguments.reserve(ordered.size());
                for (const Edge& edge : ordered) arguments.push_back(values.at(edge.left));

                std::unordered_map<NodeId, uint32_t> occurrences;
                for (const Edge& edge : ordered) ++occurrences[edge.left];
                BuiltinContext context;
                std::set<NodeId> hinted_sources;
                for (size_t index = 0; index < ordered.size(); ++index) {
                    NodeId source_id = ordered[index].left;
                    if (hinted_sources.count(source_id) || sinks.count(source_id)) continue;
                    const Node& source = graph.nodes[source_id];
                    const Value& value = values.at(source_id);
                    if (!value_is_handle(value) || source.memory == nullptr) continue;
                    if (source.used_outgoing_edges + occurrences[source_id] != source.outgoing_edges) continue;
                    context.releasable.push_back({index, value.data.nat});
                    hinted_sources.insert(source_id);
                }

                BuiltinResult result = builtin_call(*function, arguments, *memory, *graph.names, &context);
                std::optional<NodeId> transferred_source;
                if (result.transferred_argument) {
                    if (*result.transferred_argument >= ordered.size()) throw std::logic_error("builtin returned an invalid transferred argument index");
                    transferred_source = ordered[*result.transferred_argument].left;
                }

                NodeId target = routes.rets[call].right;
                auto old = values.find(target);
                if (old != values.end() && value_is_handle(old->second) && graph.nodes[target].memory
                    && (!transferred_source || target != *transferred_source)) {
                    memory->release(old->second.data.nat);
                }
                values[target] = result.value;
                graph.nodes[target].memory = value_is_handle(result.value) ? memory->pointer_for(result.value.data.nat) : nullptr;

                for (const Edge& edge : ordered) {
                    Node& source = graph.nodes[edge.left];
                    ++source.used_outgoing_edges;
                    if (source.used_outgoing_edges > source.outgoing_edges) {
                        throw std::runtime_error("runtime outgoing-edge accounting underflow/overflow on node " + best_name(graph, edge.left));
                    }
                }
                std::set<NodeId> released_sources;
                for (const Edge& edge : ordered) {
                    NodeId source_id = edge.left;
                    if (!released_sources.insert(source_id).second) continue;
                    if (transferred_source && source_id == *transferred_source) {
                        if (source_id != target) graph.nodes[source_id].memory = nullptr;
                        continue;
                    }
                    release_node_memory(graph, source_id, values, *memory, sinks);
                }
                release_node_memory(graph, target, values, *memory, sinks);

                it = pending.erase(it);
                progress = true;
            }
            if (!progress) throw std::runtime_error("execution stalled on unresolved call dependencies");
        }

        return {std::move(values), std::move(sinks_vector), std::move(memory)};
    }

    static Graph runtime_graph(const std::string&file,Program&program,const RunType&r,Interner&names,TypeRegistry&registry){
        std::vector<const Type*>decls;
        for(auto&u:program.universes)for(auto&t:u.types)if(!t.union_template&&t.full==r.name&&(!t.function()||t.return_all))decls.push_back(&t);
        std::vector<Graph>gs;
        for(const Type*t:decls){
            if(t->return_all){
                Builder b(file,program,names,registry);
                auto fb=b.build_function(*t);
                for(auto[name,id]:fb.inputs)if(fb.graph.has(id))fb.graph.add_input_name(id,name);
                gs.push_back(std::move(fb.graph));
            }else{
                auto vs=build_variants(file,program,*t,names,registry);
                gs.insert(gs.end(),std::make_move_iterator(vs.begin()),std::make_move_iterator(vs.end()));
        }}if(gs.size()!=1)throw Error(file,r.span,"run '"+std::string(names.str(r.name))+"' requires exactly one resolved executable graph variant, but "+std::to_string(gs.size())+" are available");
        Graph g=std::move(gs[0]);
        auto routes=call_routes(g);
        std::vector<Edge>redges;
        for(auto&[i,es]:routes.args){
            (void)i;
            redges.insert(redges.end(),es.begin(),es.end());
        }for(auto&[i,e]:routes.rets){
            (void)i;
            redges.push_back(e);
        }std::unordered_map<NodeId,int> indeg;
        for(NodeId i:g.ids())indeg[i]=0;
        for(auto&e:redges)++indeg[e.right];
        std::vector<NodeId>q;
        for(auto[i,d]:indeg)if(d==0)q.push_back(i);
        size_t seen=0;
        while(!q.empty()){
            NodeId i=q.back();
            q.pop_back();
            ++seen;
            for(auto&e:redges)if(e.left==i&&--indeg[e.right]==0)q.push_back(e.right);
        }if(seen!=g.ids().size())throw Error(file,r.span,"cannot run '"+std::string(names.str(r.name))+"': directed cycle detected; execution graphs must be acyclic",graph_diagnostic(g));
        return g;
    }

    static void print_program(Program&program,const std::string&file,Interner&names,TypeRegistry&registry){
        Builder builder(file,program,names,registry);
        for(size_t ui=0;ui<program.universes.size();++ui){
            auto&u=program.universes[ui];
            std::vector<const Type*>visible;
            for(auto&t:u.types)if(!t.synthetic_variant&&!is_literal(names.str(t.name)))visible.push_back(&t);
            if(ui)std::cout<<"\n";
            std::cout<<ansi::BOLD<<kw("universe")<<" "<<green(names.str(u.name))<<ansi::RESET<<"\n";
            for(const Type*t:visible){
                std::cout<<"\n"<<kw("def")<<" "<<green(names.str(t->name))<<"\n";
                if(t->function()){
                    auto fb=builder.build_function(*t);
                    auto fi=builder.function_input(*t);
                    std::cout<<kw("input")<<"\n";
                    std::set<NodeId> inids;
                    for(auto[k,v]:fi.second){
                        (void)k;
                        inids.insert(v);
                    }print_graph(fi.first,4,inids);
                    std::cout<<kw("return")<<"\n";
                    std::set<NodeId>outids;
                    for(auto[k,v]:fb.outputs){
                        (void)k;
                        if(fb.graph.has(v))outids.insert(v);
                    }print_graph(fb.graph,4,outids);
                }else{
                    auto vs=build_variants(file,program,*t,names,registry);
                    for(size_t vi=0;vi<vs.size();++vi){
                        if(vs.size()>1)std::cout<<gray("[variant "+std::to_string(vi+1)+"/"+std::to_string(vs.size())+"]")<<"\n";
                        print_graph(vs[vi]);
    }}}}}
    static void console_runs(const std::string&file,Program&program,Interner&names,TypeRegistry&registry){
        for(auto&r:program.runs){
            Graph g=runtime_graph(file,program,r,names,registry);
            std::unordered_map<NameId,std::string>raw;
            auto roots=runtime_roots(g);
            std::set<NodeId>rootset(roots.begin(),roots.end());
            auto gn=graph_names(g,rootset);
            std::cout<<"\n"<<kw("run")<<" "<<typefmt(names.str(r.name))<<"\n";
            for(NodeId i:roots){
                if(node_has_default_literal(g,i))continue;
                auto[typ,conv]=root_converter(g,i);
                (void)conv;
                std::cout<<names.str(gn[i])<<" ("<<names.str(typ)<<"): "<<std::flush;
                std::string line;
                if(!std::getline(std::cin,line))throw std::runtime_error("input ended while reading "+std::string(names.str(gn[i])));
                raw[gn[i]]=line;
            }auto result=execute_graph(g,raw);
            for(NodeId i:result.sinks)std::cout<<cyan(best_name(g,i))<<" "<<gray("=")<<" "<<value_string(result.values.at(i), *result.memory)<<"\n";
    }}

    static std::string html_escape(std::string_view text) {
        std::string out;
        out.reserve(text.size());
        for (char c : text) {
            switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += "&quot;"; break;
                case '\'': out += "&#39;"; break;
                default: out += c; break;
            }
        }
        return out;
    }

    static std::string json_escape(std::string_view text) {
        std::ostringstream out;
        out << '"';
        for (unsigned char c : text) {
            switch (c) {
                case '"': out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\b': out << "\\b"; break;
                case '\f': out << "\\f"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default:
                    if (c < 0x20) {
                        out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                            << static_cast<int>(c) << std::dec << std::setfill(' ');
                    } else {
                        out << static_cast<char>(c);
                    }
                    break;
            }
        }
        out << '"';
        return out.str();
    }

    static std::string html_type_name(std::string_view qualified) {
        size_t pos = qualified.rfind("::");
        std::string_view universe = pos == std::string_view::npos ? std::string_view{} : qualified.substr(0, pos + 2);
        std::string_view name = pos == std::string_view::npos ? qualified : qualified.substr(pos + 2);
        std::string cls = is_literal(name) ? "literal" : "type";
        if (universe.empty()) {
            return "<span class=\"" + cls + "\">" + html_escape(name) + "</span>";
        }
        return "<span class=\"qualifier\">" + html_escape(universe) + "</span><span class=\"" + cls + "\">" + html_escape(name) + "</span>";
    }

    static std::string html_graph(const Graph& g, std::optional<std::set<NodeId>> subset = std::nullopt) {
        std::set<NodeId> ids;
        if (subset) {
            ids = *subset;
        } else {
            for (NodeId id : g.ids()) ids.insert(id);
        }
        if (ids.empty()) return "<div class=\"empty\">∅</div>";

        auto names = graph_names(g, ids);
        std::vector<NodeId> order(ids.begin(), ids.end());
        std::sort(order.begin(), order.end(), [&](NodeId a, NodeId b) {
            std::string an = names[a] == NoName ? "#" + std::to_string(a) : std::string(g.names->str(names[a]));
            std::string bn = names[b] == NoName ? "#" + std::to_string(b) : std::string(g.names->str(names[b]));
            return an < bn;
        });

        std::ostringstream out;
        for (NodeId id : order) {
            std::string display = names[id] == NoName ? "#" + std::to_string(id) : std::string(g.names->str(names[id]));
            out << "<div class=\"statement\"><span class=\"name\">" << html_escape(display)
                << "</span> <span class=\"muted\">:</span> ";
            auto type_names = g.types->names_for(g.nodes[id].types);
            for (size_t i = 0; i < type_names.size(); ++i) {
                if (i) out << " <span class=\"muted\">|</span> ";
                out << html_type_name(g.names->str(type_names[i]));
            }
            out << "</div>";
        }

        auto show = [&](NodeId id) {
            auto it = names.find(id);
            if (it == names.end() || it->second == NoName) return "#" + std::to_string(id);
            return std::string(g.names->str(it->second));
        };
        for (const Edge& edge : g.edges) {
            if (!ids.count(edge.left) || !ids.count(edge.right)) continue;
            out << "<div class=\"statement\"><span class=\"name\">" << html_escape(show(edge.left))
                << "</span> <span class=\"relation\">" << html_escape(g.names->str(edge.tag))
                << "</span> <span class=\"name\">" << html_escape(show(edge.right)) << "</span></div>";
        }
        return out.str();
    }

    static std::string graph_data_json(const Graph& g, std::optional<std::set<NodeId>> subset = std::nullopt) {
        std::set<NodeId> ids;
        if (subset) {
            ids = *subset;
        } else {
            for (NodeId id : g.ids()) ids.insert(id);
        }
        auto display_names = graph_names(g, ids);
        std::ostringstream out;
        out << "{\"nodes\":[";
        bool first = true;
        for (NodeId id : ids) {
            if (!first) out << ",";
            first = false;
            std::string display = display_names[id] == NoName
                ? "#" + std::to_string(id)
                : std::string(g.names->str(display_names[id]));
            auto type_names = g.types->names_for(g.nodes[id].types);
            std::vector<std::string> type_labels;
            for (NameId type_name : type_names) type_labels.emplace_back(g.names->str(type_name));
            std::string node_label = display;
            if (!type_labels.empty()) node_label += " : " + join(type_labels, " | ");
            out << "{\"key\":" << json_escape(std::to_string(id))
                << ",\"label\":" << json_escape(node_label)
                << ",\"types\":[";
            for (size_t i = 0; i < type_names.size(); ++i) {
                if (i) out << ",";
                out << json_escape(std::string(g.names->str(type_names[i])));
            }
            out << "],\"aliases\":[";
            bool first_alias = true;
            for (const Alias& alias : g.nodes[id].names) {
                if (alias.name == NoName) continue;
                std::string alias_name(g.names->str(alias.name));
                if (alias_name.empty()) continue;
                if (!first_alias) out << ",";
                first_alias = false;
                if (alias.theory != NoName && alias_name.find("::") == std::string::npos) {
                    out << json_escape(std::string(g.names->str(alias.theory)) + "::" + alias_name);
                } else {
                    out << json_escape(alias_name);
                }
            }
            out << "]}";
        }
        out << "],\"edges\":[";
        first = true;
        size_t edge_index = 0;
        for (const Edge& edge : g.edges) {
            if (!ids.count(edge.left) || !ids.count(edge.right)) continue;
            if (!first) out << ",";
            first = false;
            out << "{\"key\":" << json_escape("e" + std::to_string(edge_index++))
                << ",\"source\":" << json_escape(std::to_string(edge.left))
                << ",\"target\":" << json_escape(std::to_string(edge.right))
                << ",\"label\":" << json_escape(std::string(g.names->str(edge.tag))) << "}";
        }
        out << "]}";
        return out.str();
    }

    static std::string graph_button_from_payload(std::string payload, std::string_view title = "View graph") {
        return "<button class=\"graph-btn\" type=\"button\" title=\"" + html_escape(title)
            + "\" aria-label=\"" + html_escape(title) + "\" data-graph=\""
            + html_escape(payload) + "\">◉</button>";
    }

    static std::string lazy_graph_button_html(uint64_t uid, std::string_view title = "View graph") {
        return "<button class=\"graph-btn\" type=\"button\" title=\"" + html_escape(title)
            + "\" aria-label=\"" + html_escape(title) + "\" data-definition-uid=\""
            + std::to_string(uid) + "\">◉</button>";
    }

    static std::string graph_button_html(
        const Graph& g,
        std::optional<std::set<NodeId>> subset = std::nullopt,
        std::string_view pane_title = "Graph"
    ) {
        std::string payload = "{\"mode\":\"single\",\"panes\":[{\"title\":"
            + json_escape(std::string(pane_title)) + ",\"graph\":" + graph_data_json(g, subset) + "}]}";
        return graph_button_from_payload(std::move(payload));
    }


    static std::optional<std::string> workspace_relative_path(
        const std::filesystem::path& root,
        std::string_view file
    );

    static std::string html_error_message(
        std::string_view message,
        const std::unordered_map<std::string, std::string>& sources,
        const std::filesystem::path& workspace_root
    ) {
        struct LinkTarget {
            std::string absolute;
            std::string relative;
        };
        std::vector<LinkTarget> targets;
        for (const auto& [source_file, ignored] : sources) {
            (void)ignored;
            auto relative = workspace_relative_path(workspace_root, source_file);
            if (!relative || std::filesystem::path(*relative).extension() != ".gs") continue;
            targets.push_back({source_file, *relative});
        }
        std::sort(targets.begin(), targets.end(), [](const LinkTarget& a, const LinkTarget& b) {
            return a.absolute.size() > b.absolute.size();
        });

        std::ostringstream out;
        size_t pos = 0;
        while (pos < message.size()) {
            const LinkTarget* chosen = nullptr;
            size_t chosen_pos = std::string_view::npos;
            for (const auto& target : targets) {
                size_t at = message.find(target.absolute, pos);
                if (at == std::string_view::npos) continue;
                size_t colon = at + target.absolute.size();
                if (colon >= message.size() || message[colon] != ':') continue;
                size_t digit = colon + 1;
                if (digit >= message.size() || !std::isdigit(static_cast<unsigned char>(message[digit]))) continue;
                if (!chosen || at < chosen_pos) {
                    chosen = &target;
                    chosen_pos = at;
                }
            }
            if (!chosen) {
                out << html_escape(message.substr(pos));
                break;
            }
            out << html_escape(message.substr(pos, chosen_pos - pos));
            size_t number_begin = chosen_pos + chosen->absolute.size() + 1;
            size_t number_end = number_begin;
            while (number_end < message.size() && std::isdigit(static_cast<unsigned char>(message[number_end]))) ++number_end;
            uint32_t line = static_cast<uint32_t>(std::stoul(std::string(message.substr(number_begin, number_end - number_begin))));
            size_t display_end = number_end;
            if (display_end < message.size() && message[display_end] == ':') {
                size_t column_begin = display_end + 1;
                size_t column_end = column_begin;
                while (column_end < message.size() && std::isdigit(static_cast<unsigned char>(message[column_end]))) ++column_end;
                if (column_end > column_begin) display_end = column_end;
            }
            std::string display = chosen->relative + std::string(message.substr(number_begin - 1, display_end - (number_begin - 1)));
            out << "<button class=\"source-open inline-error-source\" data-file=\"" << html_escape(chosen->relative)
                << "\" data-line=\"" << line << "\" title=\"Open " << html_escape(chosen->relative)
                << "\">" << html_escape(display) << "</button>";
            pos = display_end;
        }
        return out.str();
    }

    static std::string html_error(const Error& error, const std::unordered_map<std::string, std::string>& sources, const std::filesystem::path& workspace_root) {
        std::ostringstream out;
        out << "<div class=\"error-card\"><strong>error</strong><pre class=\"error-message\">" << html_error_message(error.what(), sources, workspace_root) << "</pre>";
        auto relative = workspace_relative_path(workspace_root, error.file);
        if (relative && std::filesystem::path(*relative).extension() == ".gs") {
            out << "<button class=\"source-open error-location\" data-file=\"" << html_escape(*relative)
                << "\" data-line=\"" << error.span.start.line << "\" title=\"Open " << html_escape(*relative) << "\">"
                << html_escape(*relative) << ":" << error.span.start.line << ":" << error.span.start.column << "</button>";
        } else {
            out << "<small>" << html_escape(error.file) << ":" << error.span.start.line << ":" << error.span.start.column << "</small>";
        }

        auto source = sources.find(error.file);
        if (source != sources.end()) {
            std::istringstream lines(source->second);
            std::string line;
            uint32_t line_number = 1;
            while (line_number < error.span.start.line && std::getline(lines, line)) ++line_number;
            if (line_number == error.span.start.line && std::getline(lines, line)) {
                size_t start = error.span.start.column ? error.span.start.column - 1 : 0;
                size_t width = 1;
                if (error.span.end.line == error.span.start.line && error.span.end.column > error.span.start.column) {
                    width = error.span.end.column - error.span.start.column;
                }
                if (start > line.size()) start = line.size();
                width = std::min(width, std::max<size_t>(1, line.size() - start));
                out << "<pre class=\"error-source\"><span>" << std::setw(4) << error.span.start.line << " | </span>"
                    << html_escape(line) << "\n<span>     | </span>" << std::string(start, ' ')
                    << "<b>" << std::string(width, '^') << "</b></pre>";
            }
        }
        if (!error.graph_context.empty()) {
            out << "<pre class=\"error-source\">" << html_escape(error.graph_context) << "</pre>";
        }
        out << "</div>";
        return out.str();
    }

    static std::string html_exception(std::string_view message) {
        return "<div class=\"error-card\"><strong>error</strong><pre class=\"error-message\">" + html_escape(message) + "</pre></div>";
    }

    static bool path_is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
        auto root_it = root.begin();
        auto candidate_it = candidate.begin();
        for (; root_it != root.end(); ++root_it, ++candidate_it) {
            if (candidate_it == candidate.end() || *root_it != *candidate_it) return false;
        }
        return true;
    }

    static std::filesystem::path workspace_path(
        const std::filesystem::path& root,
        std::string_view relative,
        bool require_exists
    ) {
        std::filesystem::path rel(relative.empty() ? "." : std::string(relative));
        if (rel.is_absolute()) throw std::runtime_error("workspace paths must be relative");

        std::filesystem::path candidate = std::filesystem::weakly_canonical(root / rel);
        if (!path_is_within(root, candidate)) {
            throw std::runtime_error("path escapes the served working directory");
        }
        if (require_exists && !std::filesystem::exists(candidate)) {
            throw std::runtime_error("path does not exist: " + std::string(relative));
        }
        return candidate;
    }

    static std::optional<std::string> workspace_relative_path(
        const std::filesystem::path& root,
        std::string_view file
    ) {
        if (file.empty() || file == "<builtin>") return std::nullopt;
        try {
            std::filesystem::path candidate = std::filesystem::weakly_canonical(std::filesystem::path(file));
            if (!path_is_within(root, candidate)) return std::nullopt;
            std::filesystem::path rel = candidate.lexically_relative(root);
            if (rel.empty()) return std::nullopt;
            return rel.generic_string();
        } catch (...) {
            return std::nullopt;
        }
    }

    static std::string source_button_html(
        const std::filesystem::path& root,
        std::string_view file,
        uint32_t line
    ) {
        auto relative = workspace_relative_path(root, file);
        if (!relative || std::filesystem::path(*relative).extension() != ".gs") {
            return "<span class=\"source-file\">" + html_escape(std::filesystem::path(file).filename().string()) + "</span>";
        }
        std::ostringstream out;
        out << "<button class=\"source-open\" data-file=\"" << html_escape(*relative)
            << "\" data-line=\"" << line << "\" title=\"Open " << html_escape(*relative) << "\">"
            << "file: " << html_escape(*relative) << "</button>";
        return out.str();
    }

    using EditorBuffers = std::unordered_map<std::string, std::string>;

    static void require_gs_editor_path(const std::filesystem::path& path);

    static void install_editor_buffers(
        Loader& loader,
        const std::filesystem::path& root,
        const EditorBuffers& buffers
    ) {
        for (const auto& [relative, source] : buffers) {
            std::filesystem::path path = workspace_path(root, relative, false);
            require_gs_editor_path(path);
            loader.source_overrides[std::filesystem::absolute(path).lexically_normal().string()] = source;
        }
    }

    static void append_search_token(std::string& search, std::string_view token) {
        if (token.empty()) return;
        search += " ";
        search += token;
        size_t split = token.rfind("::");
        if (split != std::string_view::npos && split + 2 < token.size()) {
            search += " ";
            search += token.substr(split + 2);
        }
    }

    static void append_graph_search_tokens(std::string& search, const Graph& graph) {
        for (NodeId id : graph.ids()) {
            for (NameId type_name : graph.types->names_for(graph.nodes[id].types)) {
                append_search_token(search, graph.names->str(type_name));
            }
            for (const Alias& alias : graph.nodes[id].names) {
                if (alias.name == NoName) continue;
                append_search_token(search, graph.names->str(alias.name));
                if (alias.theory != NoName) {
                    std::string_view alias_name = graph.names->str(alias.name);
                    if (alias_name.find("::") == std::string_view::npos) {
                        append_search_token(search, std::string(graph.names->str(alias.theory)) + "::" + std::string(alias_name));
                    }
                }
            }
        }
    }

    static std::vector<std::string> run_input_aliases(const Graph& graph, NodeId id, std::string_view display) {
        std::vector<std::string> aliases;
        auto add = [&](std::string value) {
            if (value.empty()) return;
            if (std::find(aliases.begin(), aliases.end(), value) == aliases.end()) aliases.push_back(std::move(value));
        };
        add(std::string(display));
        for (NameId input_name : graph.nodes[id].input_names) {
            if (input_name != NoName) add(std::string(graph.names->str(input_name)));
        }
        for (const Alias& alias : graph.nodes[id].names) {
            if (alias.name == NoName) continue;
            std::string alias_name(graph.names->str(alias.name));
            add(alias_name);
            if (alias.theory != NoName && alias_name.find("::") == std::string::npos) {
                add(std::string(graph.names->str(alias.theory)) + "::" + alias_name);
            }
        }
        return aliases;
    }

    static std::string inferred_html(
        Program& program,
        const std::string& file,
        Interner& names,
        TypeRegistry& registry,
        const std::filesystem::path& workspace_root,
        const std::unordered_map<std::string, std::string>& sources
    ) {
        std::ostringstream out;

        for (auto& universe : program.universes) {
            std::ostringstream universe_html;
            bool universe_failed = false;
            bool universe_has_edited_definition = false;
            size_t visible_definitions = 0;

            for (auto& type : universe.types) {
                if (type.synthetic_variant || is_literal(names.str(type.name))) continue;
                ++visible_definitions;

                std::string type_file = type.source_file.empty() ? file : type.source_file;
                bool edited_file_definition = false;
                try {
                    edited_file_definition =
                        std::filesystem::weakly_canonical(std::filesystem::absolute(type_file))
                        == std::filesystem::weakly_canonical(std::filesystem::absolute(file));
                } catch (...) {
                    edited_file_definition = std::filesystem::path(type_file).lexically_normal()
                        == std::filesystem::path(file).lexically_normal();
                }
                if (edited_file_definition) universe_has_edited_definition = true;
                std::string qualified = std::string(names.str(type.full));
                std::string origin = workspace_relative_path(workspace_root, type_file).value_or(type_file);
                std::string search_text = std::string(names.str(universe.name)) + " "
                    + std::string(names.str(type.name)) + " " + qualified + " " + origin;

                // Keep inference lightweight: definition bodies and graph payloads are fetched on demand.
                universe_html << "<details class=\"definition-card\" data-uid=\"" << type.uid << "\" data-file=\"" << html_escape(origin)
                    << "\" data-source=\"" << html_escape(type_file) << "\" data-local=\"" << (edited_file_definition ? "1" : "0")
                    << "\" data-line=\"" << type.span.start.line << "\" data-search=\"" << html_escape(search_text)
                    << "\"" << (edited_file_definition ? " open" : "") << ">"
                    << "<summary class=\"definition-heading\"><span><span class=\"keyword\">def</span> <span class=\"type\">"
                    << html_escape(names.str(type.name)) << "</span></span><span class=\"definition-actions\">"
                    << lazy_graph_button_html(type.uid) << source_button_html(workspace_root, type_file, type.span.start.line)
                    << "</span></summary><div class=\"definition-content\" data-lazy-definition=\"" << type.uid
                    << "\"><span class=\"muted\">Open to load preview…</span></div></details>";
            }

            if (visible_definitions == 0) continue;
            out << "<details class=\"universe\" data-universe=\"" << html_escape(names.str(universe.name)) << "\""
                << ((universe_failed || universe_has_edited_definition) ? " open" : "")
                << "><summary><span class=\"keyword\">universe</span> <span class=\"type\">"
                << html_escape(names.str(universe.name)) << "</span></summary>"
                << universe_html.str() << "</details>";
        }

        for (size_t run_index = 0; run_index < program.runs.size(); ++run_index) {
            const RunType& run = program.runs[run_index];
            std::string run_file = run.source_file.empty() ? file : run.source_file;
            std::string origin = workspace_relative_path(workspace_root, run_file).value_or(run_file);
            std::string search_text = "run " + std::string(names.str(run.name)) + " " + origin;
            try {
                Graph graph = runtime_graph(run_file, program, run, names, registry);
                auto roots = runtime_roots(graph);
                std::set<NodeId> root_ids(roots.begin(), roots.end());
                auto root_names = graph_names(graph, root_ids);
                append_graph_search_tokens(search_text, graph);

                out << "<article class=\"run-card\" data-run=\"" << run_index << "\" data-search=\""
                    << html_escape(search_text) << "\"><h3 class=\"definition-heading\"><span><span class=\"keyword\">run</span> "
                    << html_type_name(names.str(run.name)) << "</span><span class=\"definition-actions\">"
                    << graph_button_html(graph, std::nullopt, "Run graph")
                    << source_button_html(workspace_root, run_file, run.span.start.line)
                    << "</span></h3><div class=\"run-inputs\">";
                for (NodeId root : roots) {
                    if (node_has_default_literal(graph, root)) continue;
                    auto [type_name, converter] = root_converter(graph, root);
                    (void)converter;
                    NameId display_id = root_names[root];
                    std::string display = display_id == NoName ? best_name(graph, root) : std::string(names.str(display_id));
                    std::vector<std::string> aliases = run_input_aliases(graph, root, display);
                    out << "<label class=\"run-field\"><span>" << html_escape(display) << " <small>"
                        << html_escape(names.str(type_name)) << "</small></span><input data-name=\""
                        << html_escape(display) << "\" data-aliases=\""
                        << html_escape(join(aliases, "\n")) << "\"></label>";
                }
                out << "</div><button class=\"run-exec\">Run</button><div class=\"run-results\"></div></article>";
            } catch (const Error& error) {
                out << html_error(error, sources, workspace_root);
            } catch (const std::exception& error) {
                out << html_exception(error.what());
            }
        }

        return out.str();
    }

    static Program program_before_error(const Program& program, const Error& error) {
        uint32_t cutoff = error.span.start.offset;
        bool found_containing_declaration = false;

        auto same_error_file = [&](std::string_view source_file) {
            if (source_file.empty()) return false;
            return std::filesystem::path(source_file).lexically_normal() == std::filesystem::path(error.file).lexically_normal();
        };

        for (const Universe& universe : program.universes) {
            for (const Type& type : universe.types) {
                if (same_error_file(type.source_file) && type.span.start.offset <= error.span.start.offset) {
                    if (!found_containing_declaration || type.span.start.offset > cutoff) {
                        cutoff = type.span.start.offset;
                        found_containing_declaration = true;
                    }
                }
            }
        }
        for (const RunType& run : program.runs) {
            if (same_error_file(run.source_file) && run.span.start.offset <= error.span.start.offset) {
                if (!found_containing_declaration || run.span.start.offset > cutoff) {
                    cutoff = run.span.start.offset;
                    found_containing_declaration = true;
                }
            }
        }

        Program prefix;
        prefix.imports = program.imports;
        for (const Universe& universe : program.universes) {
            Universe kept{universe.name, {}};
            for (const Type& type : universe.types) {
                if (!same_error_file(type.source_file) || type.span.start.offset < cutoff) kept.types.push_back(type);
            }
            if (!kept.types.empty() || prefix.universes.empty()) prefix.universes.push_back(std::move(kept));
        }
        if (prefix.universes.empty()) prefix.universes.push_back({});
        for (const RunType& run : program.runs) {
            if (!same_error_file(run.source_file) || run.span.start.offset < cutoff) prefix.runs.push_back(run);
        }
        return prefix;
    }

    static std::string partial_inferred_html(
        Program program,
        const Error& original_error,
        const std::string& file,
        Interner& names,
        uint64_t& next_uid,
        const std::filesystem::path& workspace_root,
        const std::unordered_map<std::string, std::string>& sources
    ) {
        std::ostringstream out;
        // Resolver errors can cascade: the declaration immediately before the original
        // failure may itself depend on the failing declaration. Keep trimming only the
        // declaration responsible for the next resolver error until the largest
        // resolvable prefix remains, rather than dropping the entire universe view.
        Program candidate = std::move(program);
        for (size_t attempts = 0; attempts < 256; ++attempts) {
            try {
                Program resolved = candidate;
                Resolver(file, resolved, names, next_uid).resolve();
                TypeRegistry registry(names);
                out << inferred_html(resolved, file, names, registry, workspace_root, sources);
                break;
            } catch (const Error& partial_error) {
                Program smaller = program_before_error(candidate, partial_error);
                size_t before = candidate.runs.size();
                size_t after = smaller.runs.size();
                for (const Universe& universe : candidate.universes) before += universe.types.size();
                for (const Universe& universe : smaller.universes) after += universe.types.size();
                if (after >= before) break;
                candidate = std::move(smaller);
            } catch (...) {
                break;
            }
        }
        out << html_error(original_error, sources, workspace_root);
        return out.str();
    }

    static std::string editor_definition_data(
        const std::string& source,
        const std::string& file,
        uint64_t uid,
        bool graph_only,
        const EditorBuffers& buffers,
        const std::filesystem::path& workspace_root
    ) {
        Interner names;
        Loader loader(names);
        install_editor_buffers(loader, workspace_root, buffers);
        Program program = loader.load(file, source);
        Resolver(file, program, names, loader.next_uid).resolve();
        TypeRegistry registry(names);
        Type* target = nullptr;
        for (auto& universe : program.universes) {
            for (auto& type : universe.types) {
                if (type.uid == uid) {
                    target = &type;
                    break;
                }
            }
            if (target) break;
        }
        if (!target) throw std::runtime_error("definition is no longer available; refresh inference");
        std::string type_file = target->source_file.empty() ? file : target->source_file;
        Builder builder(type_file, program, names, registry);
        if (target->function()) {
            auto built = builder.build_function(*target);
            auto input = builder.function_input(*target);
            std::set<NodeId> input_ids;
            for (const auto& [name, id] : input.second) {
                (void)name;
                input_ids.insert(id);
            }
            std::set<NodeId> output_ids;
            for (const auto& [name, id] : built.outputs) {
                (void)name;
                if (built.graph.has(id)) output_ids.insert(id);
            }
            if (graph_only) {
                bool has_input = !input_ids.empty();
                bool has_output = !output_ids.empty();
                if (has_input && has_output) {
                    return "{\"mode\":\"split\",\"panes\":[{\"title\":\"Input\",\"graph\":"
                        + graph_data_json(input.first, input_ids) + "},{\"title\":\"Return\",\"graph\":"
                        + graph_data_json(built.graph, output_ids) + "}]}";
                }
                if (has_input) return "{\"mode\":\"single\",\"panes\":[{\"title\":\"Input\",\"graph\":" + graph_data_json(input.first, input_ids) + "}]}";
                if (has_output) return "{\"mode\":\"single\",\"panes\":[{\"title\":\"Return\",\"graph\":" + graph_data_json(built.graph, output_ids) + "}]}";
                return "{\"mode\":\"single\",\"panes\":[]}";
            }
            return "<div class=\"section-keyword\">input</div><div class=\"function-body\">" + html_graph(input.first, input_ids)
                + "</div><div class=\"section-keyword\">return</div><div class=\"function-body\">" + html_graph(built.graph, output_ids) + "</div>";
        }
        auto variants = build_variants(type_file, program, *target, names, registry);
        if (graph_only) {
            if (variants.empty()) return "{\"mode\":\"single\",\"panes\":[]}";
            return "{\"mode\":\"single\",\"panes\":[{\"title\":\"Type\",\"graph\":" + graph_data_json(variants.front()) + "}]}";
        }
        std::ostringstream body;
        for (size_t i = 0; i < variants.size(); ++i) {
            if (variants.size() > 1) body << "<div class=\"variant-heading\"><span class=\"muted\">variant " << (i + 1) << "/" << variants.size() << "</span></div>";
            body << html_graph(variants[i]);
        }
        return body.str();
    }

    static std::string process_editor_source(
        const std::string& source,
        const std::string& file,
        const EditorBuffers& buffers,
        const std::filesystem::path& workspace_root
    ) {
        Interner names;
        Loader loader(names);
        install_editor_buffers(loader, workspace_root, buffers);

        Program program;
        try {
            program = loader.load(file, source);
        } catch (const Error& error) {
            if (loader.last_partial_program) {
                return partial_inferred_html(*loader.last_partial_program, error, file, names, loader.next_uid, workspace_root, loader.sources);
            }
            return html_error(error, loader.sources, workspace_root);
        } catch (const std::exception& error) {
            return html_exception(error.what());
        }

        Program unresolved = program;
        try {
            Resolver(file, program, names, loader.next_uid).resolve();
            TypeRegistry registry(names);
            return inferred_html(program, file, names, registry, workspace_root, loader.sources);
        } catch (const Error& error) {
            Program prefix = program_before_error(unresolved, error);
            return partial_inferred_html(std::move(prefix), error, file, names, loader.next_uid, workspace_root, loader.sources);
        } catch (const std::exception& error) {
            // A resolver implementation bug or resource-limit failure should not blank
            // the inference pane. Generic exceptions do not carry a source span, so
            // progressively remove the latest declarations until a resolvable program
            // remains, render that prefix, and append the original diagnostic.
            Program candidate = unresolved;
            for (size_t attempts = 0; attempts < 256; ++attempts) {
                try {
                    Program resolved = candidate;
                    Resolver(file, resolved, names, loader.next_uid).resolve();
                    TypeRegistry registry(names);
                    return inferred_html(resolved, file, names, registry, workspace_root, loader.sources)
                        + html_exception(error.what());
                } catch (...) {
                    bool removed = false;
                    for (auto uit = candidate.universes.rbegin(); uit != candidate.universes.rend(); ++uit) {
                        if (!uit->types.empty()) {
                            uit->types.pop_back();
                            removed = true;
                            break;
                        }
                    }
                    if (!removed && !candidate.runs.empty()) {
                        candidate.runs.pop_back();
                        removed = true;
                    }
                    if (!removed) break;
                }
            }
            return html_exception(error.what());
        }
    }

    static std::string execute_editor_run(
        const std::string& source,
        const std::string& file,
        size_t run_index,
        const std::unordered_map<std::string, std::string>& input_values,
        const EditorBuffers& buffers,
        const std::filesystem::path& workspace_root
    ) {
        Interner names;
        Loader loader(names);
        try {
            install_editor_buffers(loader, workspace_root, buffers);
            Program program = loader.load(file, source);
            Resolver(file, program, names, loader.next_uid).resolve();
            TypeRegistry registry(names);
            if (run_index >= program.runs.size()) throw std::runtime_error("unknown run request");

            const RunType& run = program.runs[run_index];
            std::string run_file = run.source_file.empty() ? file : run.source_file;
            Graph graph = runtime_graph(run_file, program, run, names, registry);
            auto roots = runtime_roots(graph);
            std::set<NodeId> root_ids(roots.begin(), roots.end());
            auto root_names = graph_names(graph, root_ids);
            std::unordered_map<NameId, std::string> raw;
            for (NodeId root : roots) {
                if (node_has_default_literal(graph, root)) continue;
                NameId display_id = root_names[root];
                std::string display = display_id == NoName ? best_name(graph, root) : std::string(names.str(display_id));
                auto it = input_values.find(display);
                if (it != input_values.end()) raw[names.intern(display)] = it->second;
            }

            ExecResult result = execute_graph(graph, raw);
            std::ostringstream json;
            json << "{\"results\":[";
            for (size_t i = 0; i < result.sinks.size(); ++i) {
                if (i) json << ',';
                NodeId sink = result.sinks[i];
                json << "{\"name\":" << json_escape(best_name(graph, sink))
                     << ",\"value\":" << json_escape(value_string(result.values.at(sink), *result.memory)) << '}';
            }
            json << "]}";
            return json.str();
        } catch (const Error& error) {
            std::ostringstream json;
            json << "{\"error\":" << json_escape(error.what());
            auto relative = workspace_relative_path(workspace_root, error.file);
            if (relative && std::filesystem::path(*relative).extension() == ".gs") {
                json << ",\"error_file\":" << json_escape(*relative)
                     << ",\"error_line\":" << error.span.start.line
                     << ",\"error_column\":" << error.span.start.column;
            }
            json << "}";
            return json.str();
        } catch (const std::exception& error) {
            return "{\"error\":" + json_escape(error.what()) + "}";
        }
    }

    static void require_gs_editor_path(const std::filesystem::path& path) {
        if (path.extension() != ".gs") throw std::runtime_error("the editor only opens and saves .gs files");
    }

    static std::string workspace_listing_json(
        const std::filesystem::path& root,
        std::string_view relative
    ) {
        std::filesystem::path directory = workspace_path(root, relative, true);
        if (!std::filesystem::is_directory(directory)) throw std::runtime_error("not a directory");

        struct Entry {
            std::string name;
            std::string path;
            bool directory = false;
        };
        std::vector<Entry> entries;
        for (const auto& item : std::filesystem::directory_iterator(directory)) {
            try {
                auto relative_item = workspace_relative_path(root, item.path().string());
                if (!relative_item) continue;
                bool directory = item.is_directory();
                if (!directory && item.path().extension() != ".gs") continue;
                entries.push_back({
                    item.path().filename().string(),
                    *relative_item,
                    directory
                });
            } catch (...) {
            }
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            if (a.directory != b.directory) return a.directory > b.directory;
            return a.name < b.name;
        });

        std::string current = workspace_relative_path(root, directory.string()).value_or(std::string{});
        if (current == ".") current.clear();
        std::ostringstream out;
        out << "{\"path\":" << json_escape(current) << ",\"entries\":[";
        for (size_t i = 0; i < entries.size(); ++i) {
            if (i) out << ',';
            out << "{\"name\":" << json_escape(entries[i].name)
                << ",\"path\":" << json_escape(entries[i].path)
                << ",\"directory\":" << (entries[i].directory ? "true" : "false") << '}';
        }
        out << "]}";
        return out.str();
    }

    static std::string workspace_file_json(
        const std::filesystem::path& root,
        std::string_view relative
    ) {
        std::filesystem::path path = workspace_path(root, relative, true);
        require_gs_editor_path(path);
        if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("not a regular file");
        auto normalized = workspace_relative_path(root, path.string());
        if (!normalized) throw std::runtime_error("file is outside the workspace");
        return "{\"path\":" + json_escape(*normalized) + ",\"source\":" + json_escape(read_file(path)) + "}";
    }

    static void save_workspace_file(
        const std::filesystem::path& root,
        std::string_view relative,
        std::string_view source
    ) {
        if (relative.empty()) throw std::runtime_error("missing file path");
        std::filesystem::path path = workspace_path(root, relative, false);
        require_gs_editor_path(path);
        if (std::filesystem::exists(path) && std::filesystem::is_directory(path)) {
            throw std::runtime_error("cannot save over a directory");
        }
        std::filesystem::path parent = std::filesystem::weakly_canonical(path.parent_path());
        if (!path_is_within(root, parent) || !std::filesystem::is_directory(parent)) {
            throw std::runtime_error("parent directory does not exist in the workspace");
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot open file for writing");
        output.write(source.data(), static_cast<std::streamsize>(source.size()));
        if (!output) throw std::runtime_error("failed while writing file");
    }

    static EditorBuffers editor_buffers_from_request(const httplib::Request& request) {
        std::unordered_map<std::string, std::string> files;
        std::unordered_map<std::string, std::string> sources;
        constexpr std::string_view file_prefix = "buffer_file.";
        constexpr std::string_view source_prefix = "buffer_source.";
        for (const auto& [key, value] : request.params) {
            if (key.rfind(file_prefix, 0) == 0) {
                files[key.substr(file_prefix.size())] = value;
            } else if (key.rfind(source_prefix, 0) == 0) {
                sources[key.substr(source_prefix.size())] = value;
            }
        }
        EditorBuffers buffers;
        for (const auto& [index, path] : files) {
            auto it = sources.find(index);
            if (it != sources.end() && !path.empty()) buffers[path] = it->second;
        }
        return buffers;
    }

    static std::string playground_document(
        const std::string& initial_source,
        std::string_view initial_file
    ) {
        std::ostringstream page;
        page << R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GraS</title>
<style>
:root{--bg:#101218;--panel:#171a22;--panel2:#12151c;--border:#2a3040;--text:#d9deea;--muted:#747d91;--purple:#c792ea;--green:#8bd49c;--yellow:#ffd580;--cyan:#89ddff;--red:#ff6b78}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:14px/1.65 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
button,input{font:inherit}
header{position:relative;z-index:100;height:58px;display:flex;align-items:center;gap:8px;padding:0 12px;border-bottom:1px solid var(--border);background:#101218f2;overflow:visible}
.toolbar-button,.tab-button,.tab-close,.source-open{background:#222735;color:var(--text);border:1px solid #384055;border-radius:7px;padding:6px 10px;cursor:pointer}
.toolbar-button:disabled{opacity:.45;cursor:default}
.menu{position:relative;z-index:101}.menu-button{background:transparent;color:var(--text);border:1px solid transparent;border-radius:6px;padding:6px 9px;cursor:pointer}.menu-button:hover,.menu.open>.menu-button{background:#222735;border-color:#384055}.menu-popup{display:none;position:absolute;z-index:102;top:calc(100% + 5px);left:0;min-width:245px;padding:5px;background:#171a22;border:1px solid #41495f;border-radius:8px;box-shadow:0 12px 38px #000a}.menu.open>.menu-popup{display:block}.menu-item{display:flex;width:100%;align-items:center;justify-content:space-between;gap:20px;border:0;border-radius:5px;background:transparent;color:var(--text);padding:6px 9px;text-align:left;cursor:pointer}.menu-item:hover{background:#282e3d}.menu-item:disabled{opacity:.42;cursor:default}.menu-shortcut{color:var(--muted);font-size:12px;white-space:nowrap}.menu-separator{height:1px;margin:4px 5px;background:var(--border)}
.tabs{display:flex;align-items:center;gap:5px;min-width:0;overflow:auto;margin-left:4px;flex:1}
.tab{display:flex;align-items:center;min-width:0;border:1px solid transparent;border-radius:7px;background:#151923}
.tab.active{border-color:#46506b;background:#202635}.tab-button{border:0;background:transparent;max-width:240px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.tab-close{border:0;background:transparent;padding:4px 7px;color:var(--muted)}
.tab .dirty{color:var(--yellow);margin-left:4px}.save-state{color:var(--muted);font-size:12px;white-space:nowrap}
main{display:grid;grid-template-columns:1fr 1fr;height:calc(100vh - 58px)}
.editor,.inference{min-width:0;overflow:auto}.editor{position:relative;background:var(--panel2);border-right:1px solid var(--border)}
.editwrap{position:absolute;inset:38px 0 0}.editwrap textarea,.editwrap pre{position:absolute;inset:0;margin:0;padding:20px 22px;border:0;overflow:auto;font:14px/1.65 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;tab-size:4;white-space:pre}.editwrap textarea{z-index:2;resize:none;background:transparent;color:transparent;caret-color:white;-webkit-text-fill-color:transparent;outline:none}.editwrap textarea:disabled{cursor:default}.editwrap pre{z-index:1;pointer-events:none;color:var(--text)}
.hl-k{color:var(--purple);font-weight:600}.hl-t{color:var(--green)}.hl-s{color:var(--yellow)}.hl-c{color:#596174}.hl-o{color:#f07178}
.pane-title{position:sticky;top:0;z-index:3;height:38px;padding:10px 20px 7px;background:inherit;color:var(--muted);text-transform:uppercase;letter-spacing:.12em;font-size:11px}
.editor .pane-title{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.editor-search{position:absolute;z-index:8;right:14px;top:46px;display:flex;gap:6px;align-items:center;background:#151923;border:1px solid #46506b;border-radius:8px;padding:7px;box-shadow:0 8px 30px #0008}.editor-search[hidden]{display:none}.editor-search input{width:240px;background:#0f1218;color:var(--text);border:1px solid var(--border);border-radius:6px;padding:5px 8px;outline:none}.editor-search .count{min-width:56px;color:var(--muted);font-size:11px;text-align:center}.inference{padding:0 24px 80px}.inference>.pane-title{margin:0 -24px 10px;background:var(--bg);display:flex;align-items:center;gap:12px}.inference>.pane-title span{white-space:nowrap}.panel-search{margin-left:auto;min-width:120px;width:min(320px,55%);background:#0f1218;color:var(--text);border:1px solid var(--border);border-radius:6px;padding:4px 8px;text-transform:none;letter-spacing:0;outline:none}.panel-search:focus{border-color:#46506b}
details.universe{margin:14px 0 22px}details.universe>summary{cursor:pointer;font-size:17px;font-weight:700}
details.definition-card{margin:5px 0;border:1px solid var(--border);border-radius:8px;background:var(--panel)}details.definition-card[open]{margin:10px 0 14px}article{margin:14px 0;border:1px solid var(--border);border-radius:8px;background:var(--panel)}details.definition-card[hidden],article[hidden],details[hidden]{display:none}.definition-content{padding:0 14px 12px}.definition-card>summary{padding:5px 9px;cursor:pointer;list-style:none}.definition-card[open]>summary{padding:10px 14px 8px}.definition-card>summary::-webkit-details-marker{display:none}
.definition-heading{display:flex;align-items:center;justify-content:space-between;gap:8px;margin:0}.run-card>.definition-heading{margin:0 0 10px;align-items:flex-start}.definition-heading>span{min-width:0}.definition-actions{display:flex;align-items:center;justify-content:flex-end;gap:5px;min-width:0;max-width:72%}.source-open,.source-file{font-size:11px;line-height:1.35;text-transform:none;letter-spacing:0;white-space:nowrap;max-width:min(640px,62vw);overflow:hidden;text-overflow:ellipsis}.source-open{color:var(--cyan);background:#10141c;padding:3px 7px}.source-file{color:var(--muted)}.graph-btn{flex:0 0 auto;border:1px solid #384055;background:#10141c;color:var(--yellow);border-radius:6px;padding:2px 6px;line-height:1.3;cursor:pointer}.graph-btn:hover{background:#202635}.variant-heading{display:flex;align-items:center;justify-content:space-between;margin-top:8px}
.graph-modal{position:fixed;inset:0;z-index:40;display:flex;align-items:center;justify-content:center;padding:28px;background:#080a0fd9}.graph-modal[hidden]{display:none}.graph-box{position:relative;width:min(1400px,96vw);height:min(860px,92vh);display:flex;flex-direction:column;background:var(--panel);border:1px solid #41495f;border-radius:12px;box-shadow:0 24px 80px #000d;overflow:hidden}.graph-header{height:48px;display:flex;align-items:center;gap:12px;padding:9px 12px;border-bottom:1px solid var(--border)}.graph-header .muted{flex:1}.graph-grid{display:grid;grid-template-columns:minmax(0,1fr);gap:1px;min-height:0;flex:1;background:var(--border)}.graph-grid.split{grid-template-columns:minmax(0,1fr) minmax(0,1fr)}.graph-pane{position:relative;min-width:0;min-height:0;background:var(--panel2);display:flex;flex-direction:column}.graph-pane-title{padding:8px 12px;border-bottom:1px solid var(--border);color:var(--purple);font-weight:700}.sigma-wrap{position:relative;min-height:300px;flex:1;overflow:hidden}.sigma-host{position:absolute;inset:0;z-index:1}.edge-overlay{position:absolute;inset:0;width:100%;height:100%;z-index:2;pointer-events:none}.node-label-overlay{position:absolute;inset:0;z-index:3;pointer-events:none}.node-label{position:absolute;transform:translate(-50%,-100%);margin-top:-14px;color:#f8fafc;font-weight:700;font-size:12px;text-shadow:0 1px 3px #000,0 0 5px #000;white-space:nowrap}.graph-tip{display:none;position:absolute;z-index:5;max-width:min(440px,75%);padding:9px 11px;background:#0b0e14f2;border:1px solid #465067;border-radius:8px;color:#f3f6fc;box-shadow:0 8px 28px #0008;pointer-events:none;white-space:pre-wrap;font-size:12px;line-height:1.5}.graph-tip.open{display:block}
.keyword,.section-keyword{color:var(--purple);font-weight:700}.type{color:var(--green)}.literal{color:var(--yellow)}.qualifier,.muted,small{color:var(--muted)}.name{color:var(--cyan)}.relation{color:#f07178;font-weight:700}.section-keyword{margin-top:10px}.function-body{padding-left:10px}.empty{color:var(--muted)}
.error-card{display:grid;gap:6px;margin:12px 0;padding:12px 14px;border:1px solid #ff6b7860;border-left:4px solid var(--red);border-radius:7px;background:#ff6b7810;color:#ffc2c7}.error-card strong{color:var(--red)}.error-card small{color:#a66f78}.error-message{margin:0;white-space:pre-wrap;overflow-wrap:anywhere;font:inherit;color:inherit;background:transparent}.error-location{justify-self:start;border:0;background:transparent;padding:0;color:#a66f78;text-decoration:underline;cursor:pointer;font:inherit}.inline-error-source{display:inline;border:0;background:transparent;padding:0;color:var(--cyan);text-decoration:underline;cursor:pointer;font:inherit}.error-source{overflow:auto;background:#0d1017;padding:8px 10px;border-radius:5px;color:var(--text)}.error-source span{color:var(--muted)}.error-source b{color:var(--red)}
.run-inputs{display:grid;gap:8px;margin:12px 0}.run-field{display:grid;grid-template-columns:max-content minmax(0,1fr);gap:12px;align-items:center}.run-field input{min-width:0;width:100%;background:#0f1218;color:var(--text);border:1px solid var(--border);border-radius:6px;padding:6px 8px;color:var(--text)}.run-exec{width:100%;margin:8px 0;background:var(--green);color:var(--bg);border:0;border-radius:7px;padding:7px 14px;font-weight:700;cursor:pointer}.run-results{margin-top:8px}
.file-picker-backdrop{position:fixed;inset:0;z-index:20;background:#0008;display:flex;align-items:center;justify-content:center;padding:30px}.file-picker-backdrop[hidden]{display:none}.file-picker{width:min(780px,92vw);height:min(650px,82vh);display:flex;flex-direction:column;background:var(--panel);border:1px solid #41495f;border-radius:10px;box-shadow:0 20px 70px #000c;overflow:hidden}.file-picker-header{display:flex;align-items:center;gap:8px;padding:10px;border-bottom:1px solid var(--border)}.file-picker-path{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:var(--muted)}.file-list{overflow:auto;padding:8px}.file-entry{display:flex;width:100%;align-items:center;gap:10px;background:transparent;color:var(--text);border:0;border-radius:6px;padding:7px 9px;text-align:left;cursor:pointer}.file-entry:hover{background:#232938}.file-entry .kind{width:18px;color:var(--muted)}.file-entry.directory .kind{color:var(--yellow)}
@media(max-width:850px){main{grid-template-columns:1fr;height:auto}.editor{height:50vh;border-right:0;border-bottom:1px solid var(--border)}.inference{min-height:50vh}.save-state{display:none}.graph-grid.split{grid-template-columns:1fr;grid-template-rows:minmax(300px,1fr) minmax(300px,1fr)}.graph-box{height:94vh}}
</style>
</head>
<body>
<header>
<div class="menu" id="fileMenu"><button class="menu-button" data-menu="fileMenu">File</button><div class="menu-popup">
<button class="menu-item" id="newFile"><span>New</span><span class="menu-shortcut">(Ctrl/Cmd+N)</span></button>
<button class="menu-item" id="openFile"><span>Open</span><span class="menu-shortcut">(Ctrl/Cmd+O)</span></button>
<div class="menu-separator"></div>
<button class="menu-item" id="saveFile" disabled><span>Save</span><span class="menu-shortcut">(Ctrl/Cmd+S)</span></button>
<button class="menu-item" id="saveAs" disabled><span>Save As</span><span class="menu-shortcut">(Ctrl/Cmd+Shift+S)</span></button>
<button class="menu-item" id="saveAll" disabled><span>Save All</span><span class="menu-shortcut">(Ctrl/Cmd+Alt+S)</span></button>
</div></div>
<div class="menu" id="editMenu"><button class="menu-button" data-menu="editMenu">Edit</button><div class="menu-popup">
<button class="menu-item" id="editUndo"><span>Undo</span><span class="menu-shortcut">(Ctrl/Cmd+Z)</span></button>
<button class="menu-item" id="editRedo"><span>Redo</span><span class="menu-shortcut">(Ctrl/Cmd+Shift+Z)</span></button>
<div class="menu-separator"></div>
<button class="menu-item" id="editSearch"><span>Search</span><span class="menu-shortcut">(Ctrl/Cmd+F)</span></button>
<button class="menu-item" id="editComment"><span>Comment</span><span class="menu-shortcut">(Ctrl/Cmd+/)</span></button>
<button class="menu-item" id="editUncomment"><span>Uncomment</span><span class="menu-shortcut">(Ctrl/Cmd+Shift+/)</span></button>
<div class="menu-separator"></div>
<button class="menu-item" id="editTabIn"><span>Tab In</span><span class="menu-shortcut">(Tab)</span></button>
<button class="menu-item" id="editTabOut"><span>Tab Out</span><span class="menu-shortcut">(Shift+Tab)</span></button>
</div></div>
<div class="tabs" id="tabs"></div>
<span class="save-state" id="saveState"></span>
</header>
<main>
<div class="editor"><div class="pane-title" id="activePath">no file open</div><div class="editor-search" id="editorSearch" hidden><input id="editorSearchInput" placeholder="Find in file"><span class="count" id="editorSearchCount"></span><button class="toolbar-button" id="editorSearchPrev">↑</button><button class="toolbar-button" id="editorSearchNext">↓</button><button class="toolbar-button" id="editorSearchClose">×</button></div><div class="editwrap"><pre id="highlight"><code id="highlightCode"></code></pre><textarea id="editor" spellcheck="false" data-initial-file=")HTML"
            << html_escape(initial_file) << R"HTML(" disabled>)HTML" << html_escape(initial_source) << R"HTML(</textarea></div></div>
<div class="inference"><div class="pane-title"><span>inference</span><input class="panel-search" id="panelSearch" placeholder="filter definitions"></div><div id="output"></div></div>
</main>
<div class="file-picker-backdrop" id="filePicker" hidden>
<div class="file-picker" role="dialog" aria-modal="true" aria-label="Open file">
<div class="file-picker-header"><button class="toolbar-button" id="fileUp">Up</button><div class="file-picker-path" id="pickerPath"></div><button class="toolbar-button" id="closePicker">Close</button></div>
<div class="file-list" id="fileList"></div>
</div>
</div>
<div class="graph-modal" id="graphModal" hidden>
<div class="graph-box" role="dialog" aria-modal="true" aria-label="Graph visualization">
<div class="graph-header"><span class="muted">Scroll/pinch to zoom · drag to pan · hover nodes for aliases</span><button class="toolbar-button" id="graphClose">Close</button></div>
<div class="graph-grid" id="graphGrid"></div>
</div>
</div>
<script>
const editor=document.getElementById('editor');
const highlightCode=document.getElementById('highlightCode');
const highlight=document.getElementById('highlight');
const output=document.getElementById('output');
const inference=document.querySelector('.inference');
const tabsElement=document.getElementById('tabs');
const activePathElement=document.getElementById('activePath');
const editorSearch=document.getElementById('editorSearch');const editorSearchInput=document.getElementById('editorSearchInput');const editorSearchCount=document.getElementById('editorSearchCount');
const newFileButton=document.getElementById('newFile');
const openFileButton=document.getElementById('openFile');
const saveFileButton=document.getElementById('saveFile');
const saveAsButton=document.getElementById('saveAs');
const saveAllButton=document.getElementById('saveAll');
const saveState=document.getElementById('saveState');
const panelSearch=document.getElementById('panelSearch');
const filePicker=document.getElementById('filePicker');
const fileList=document.getElementById('fileList');
const pickerPath=document.getElementById('pickerPath');
const fileUp=document.getElementById('fileUp');
const closePicker=document.getElementById('closePicker');
const tabs=[];
let activeIndex=-1;
let timer=null;
let pickerDirectory='';
let inferenceSerial=0;
function esc(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
function scan(s){let types=new Set(),rels=new Set(),ts=[],i=0;while(i<s.length){if(s[i]=='\n'||s[i]==','){ts.push(['sep',s[i]]);i++;continue}if(/\s/.test(s[i])){i++;continue}if(s[i]=='/'&&s[i+1]=='/'){let j=s.indexOf('\n',i);i=j<0?s.length:j;continue}if(s[i]=='"'){let j=i+1;while(j<s.length){if(s[j]=='\\')j+=2;else if(s[j]=='"'){j++;break}else j++}ts.push(['str',s.slice(i,j)]);i=j;continue}let m=s.slice(i).match(/^[A-Za-z_][A-Za-z0-9_.]*(?:::[A-Za-z_][A-Za-z0-9_.]*)*/);if(m){let v=m[0],k=/^(universe|namespace|def|uses|return|where|reduce|all|close|do|run|import)$/.test(v)?'kw':'name';ts.push([k,v]);i+=v.length;continue}m=s.slice(i).match(/^[~=<>!+\-*\/%^&@#$?\\]+/);if(m){ts.push(['rel',m[0]]);i+=m[0].length;continue}if(s[i]==':'||s[i]=='|'||s[i]=='('||s[i]==')'){ts.push([s[i],s[i]]);i++;continue}i++}for(let j=0;j+1<ts.length;j++)if(ts[j][0]=='kw'&&/^(type|namespace|universe)$/.test(ts[j][1])&&(ts[j+1][0]=='name'||ts[j+1][0]=='str'))types.add(ts[j+1][1]);let j=0,mode='top';while(j<ts.length){let t=ts[j];if(t[0]=='kw'){if(t[1]=='universe'||t[1]=='namespace'||t[1]=='type'){j+=2;mode=t[1]=='type'?'body':'top';continue}if(t[1]=='return'||t[1]=='where'){mode=t[1];j++;continue}if(t[1]=='run'||t[1]=='import'){mode='top';j+=2;continue}j++;continue}if(mode=='top'||t[0]!='name'){j++;continue}if(j+1<ts.length&&ts[j+1][0]==':'){j+=2;if(j<ts.length&&(ts[j][0]=='name'||ts[j][0]=='str'))j++;while(j+1<ts.length&&ts[j][0]=='|'&&(ts[j+1][0]=='name'||ts[j+1][0]=='str'))j+=2;continue}if(j+2<ts.length&&(ts[j+1][0]=='rel'||ts[j+1][0]=='name')&&(ts[j+2][0]=='name'||ts[j+2][0]=='str'||ts[j+2][0]=='kw')){let mid=ts[j+1],right=ts[j+2];if(!(mid[1]=='='&&right[0]=='kw'&&right[1]=='reduce'))rels.add(mid[1]);j+=3;continue}j++}return{types,rels}}
function isCallRelationAt(s,start,length){let depth=0,inString=false,escape=false;for(let k=0;k<start;k++){const c=s[k];if(inString){if(escape)escape=false;else if(c==='\\')escape=true;else if(c==='"')inString=false;continue;}if(c==='"'){inString=true;continue;}if(c==='/'&&s[k+1]==='/'){const nl=s.indexOf('\n',k+2);if(nl<0)return false;k=nl;continue;}if(c==='(')depth++;else if(c===')')depth=Math.max(0,depth-1);}if(depth<=0)return false;let p=start-1;while(p>=0&&/\s/.test(s[p]))p--;let n=start+length;while(n<s.length&&/\s/.test(s[n]))n++;if(p<0||n>=s.length)return false;const left=/[A-Za-z0-9_".)]/.test(s[p]);const right=/[A-Za-z_".(0-9]/.test(s[n]);return left&&right;}
function highlightEditor(){let s=editor.value,o='',i=0,{types,rels}=scan(s);const keywords=new Set(['universe','namespace','def','uses','return','where','reduce','all','close','do','run','import']);while(i<s.length){if(s[i]=='('||s[i]==')'){o+='<span class="hl-s">'+s[i]+'</span>';i++;continue}if(s[i]=='/'&&s[i+1]=='/'){let j=s.indexOf('\n',i);if(j<0)j=s.length;o+='<span class="hl-c">'+esc(s.slice(i,j))+'</span>';i=j;continue}if(s[i]=='"'){let j=i+1;while(j<s.length){if(s[j]=='\\')j+=2;else if(s[j]=='"'){j++;break}else j++}o+='<span class="hl-s">'+esc(s.slice(i,j))+'</span>';i=j;continue}let m=s.slice(i).match(/^[A-Za-z_][A-Za-z0-9_]*/);if(m){const word=m[0];if(keywords.has(word))o+='<span class="hl-k">'+word+'</span>';else if(types.has(word)||rels.has(word)||isCallRelationAt(s,i,word.length))o+='<span class="'+(types.has(word)?'hl-t':'hl-o')+'">'+esc(word)+'</span>';else o+=esc(word);i+=word.length;continue}m=s.slice(i).match(/^[~=<>!+\-*\/%^&@#$?\\]+/);if(m&&rels.has(m[0])){o+='<span class="hl-o">'+esc(m[0])+'</span>';i+=m[0].length;continue}o+=esc(s[i++])}highlightCode.innerHTML=o+'\n'}
function activeTab(){return activeIndex>=0?tabs[activeIndex]:null;}
function rememberEditor(){const tab=activeTab();if(!tab)return;tab.source=editor.value;tab.scrollTop=editor.scrollTop;tab.scrollLeft=editor.scrollLeft;tab.selectionStart=editor.selectionStart;tab.selectionEnd=editor.selectionEnd;}
function renderTabs(){tabsElement.innerHTML='';tabs.forEach((tab,index)=>{const wrap=document.createElement('div');wrap.className='tab'+(index===activeIndex?' active':'');const button=document.createElement('button');button.className='tab-button';button.title=tab.path;button.textContent=tab.path.split('/').pop()+(tab.dirty?' •':'');button.addEventListener('click',()=>activateTab(index));const close=document.createElement('button');close.className='tab-close';close.textContent='×';close.title='Close';close.addEventListener('click',event=>{event.stopPropagation();closeTab(index);});wrap.append(button,close);tabsElement.appendChild(wrap);});const current=activeTab();saveFileButton.disabled=!current;saveAsButton.disabled=!current;saveAllButton.disabled=!tabs.some(tab=>tab.dirty);activePathElement.textContent=current?current.path:'no file open';}
function setEditorFromTab(tab){if(!tab){editor.value='';editor.disabled=true;highlightEditor();output.innerHTML='';return;}editor.disabled=false;editor.value=tab.source;highlightEditor();editor.scrollTop=tab.scrollTop||0;editor.scrollLeft=tab.scrollLeft||0;const start=Math.min(tab.selectionStart||0,editor.value.length),end=Math.min(tab.selectionEnd??start,editor.value.length);editor.setSelectionRange(start,end);}
function activateTab(index,jumpLine=0){if(index<0||index>=tabs.length)return;rememberEditor();activeIndex=index;setEditorFromTab(tabs[index]);renderTabs();if(jumpLine>0)jumpToLine(jumpLine);infer();}
function closeTab(index){if(index<0||index>=tabs.length)return;const tab=tabs[index];if(tab.dirty&&!confirm('Close '+tab.path+' without saving?'))return;rememberEditor();tabs.splice(index,1);if(!tabs.length)activeIndex=-1;else if(index<activeIndex)activeIndex--;else if(index===activeIndex)activeIndex=Math.min(index,tabs.length-1);setEditorFromTab(activeTab());renderTabs();if(activeTab())infer();else{inferenceSerial++;output.innerHTML='';}}
function jumpToLine(line){const lines=editor.value.split('\n');let position=0;for(let i=1;i<line&&i<=lines.length;i++)position+=lines[i-1].length+1;editor.focus();editor.setSelectionRange(position,position);const lineHeight=parseFloat(getComputedStyle(editor).lineHeight)||23;editor.scrollTop=Math.max(0,(line-3)*lineHeight);rememberEditor();}
async function openWorkspaceFile(path,line=0){let index=tabs.findIndex(tab=>tab.path===path);if(index>=0){activateTab(index,line);return;}try{const response=await fetch('/file?path='+encodeURIComponent(path));const data=await response.json();if(data.error)throw new Error(data.error);tabs.push({path:data.path,source:data.source,dirty:false,scrollTop:0,scrollLeft:0,selectionStart:0,selectionEnd:0});activateTab(tabs.length-1,line);}catch(error){saveState.textContent=String(error);}}
async function saveTab(tab){const body=new URLSearchParams();body.set('path',tab.path);body.set('source',tab.source);const response=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body});const data=await response.json();if(data.error)throw new Error(data.error);tab.dirty=false;}
async function saveActive(){rememberEditor();const tab=activeTab();if(!tab)return;if(tab.isNew){await saveAsActive();return;}try{saveState.textContent='saving…';await saveTab(tab);saveState.textContent='saved';renderTabs();setTimeout(()=>{if(saveState.textContent==='saved')saveState.textContent='';},1000);}catch(error){saveState.textContent=String(error);}}
function unusedNewPath(){let n=1;while(true){const path=n===1?'untitled.gs':'untitled-'+n+'.gs';if(!tabs.some(tab=>tab.path===path))return path;n++;}}
function newFile(){rememberEditor();const path=unusedNewPath();tabs.push({path,source:'',dirty:true,isNew:true,scrollTop:0,scrollLeft:0,selectionStart:0,selectionEnd:0});activateTab(tabs.length-1);}
async function saveAsActive(){rememberEditor();const tab=activeTab();if(!tab)return;const proposed=tab.path||'untitled.gs';const path=prompt('Save as a .gs path relative to the served working directory:',proposed);if(path===null)return;const clean=path.trim().replace(/\\/g,'/');if(!clean)return;try{saveState.textContent='saving…';if(tabs.some(other=>other!==tab&&other.path===clean)){throw new Error('that file is already open');}const oldPath=tab.path;tab.path=clean;try{await saveTab(tab);}catch(error){tab.path=oldPath;throw error;}tab.isNew=false;saveState.textContent='saved';renderTabs();infer();setTimeout(()=>{if(saveState.textContent==='saved')saveState.textContent='';},1000);}catch(error){saveState.textContent=String(error);renderTabs();}}
async function saveEveryTab(){rememberEditor();try{saveState.textContent='saving…';for(const tab of tabs)if(tab.dirty)await saveTab(tab);saveState.textContent='saved';renderTabs();setTimeout(()=>{if(saveState.textContent==='saved')saveState.textContent='';},1000);}catch(error){saveState.textContent=String(error);}}
function appendBuffers(body){rememberEditor();tabs.forEach((tab,index)=>{body.set('buffer_file.'+index,tab.path);body.set('buffer_source.'+index,tab.source);});}
const RUN_INPUT_CACHE_KEY='gras.run-input-alias-values.v1';
let runInputCache={};
try{const saved=localStorage.getItem(RUN_INPUT_CACHE_KEY);if(saved)runInputCache=JSON.parse(saved)||{};}catch(_){runInputCache={};}
function inputAliases(input){return[...new Set((input.dataset.aliases||input.dataset.name||'').split('\n').map(x=>x.trim()).filter(Boolean))];}
function persistRunInputCache(){try{localStorage.setItem(RUN_INPUT_CACHE_KEY,JSON.stringify(runInputCache));}catch(_){}}
function cacheRunInput(input){const value=input.value;inputAliases(input).forEach(alias=>{runInputCache[alias]=value;});persistRunInputCache();}
function restoreRunInputDefaults(){output.querySelectorAll('.run-card input[data-name]').forEach(input=>{if(input.value)return;for(const alias of inputAliases(input)){if(Object.prototype.hasOwnProperty.call(runInputCache,alias)){input.value=runInputCache[alias];break;}}});}
function applyPanelFilter(){const q=panelSearch.value.trim().toLowerCase();const cards=[...output.querySelectorAll('details.definition-card[data-search],article.run-card[data-search]')];cards.forEach(card=>{const hay=((card.dataset.search||'')+' '+card.textContent).toLowerCase();card.hidden=!!q&&!hay.includes(q);});output.querySelectorAll('details.universe').forEach(group=>{const definitions=[...group.querySelectorAll('details.definition-card[data-search]')];group.hidden=!!q&&definitions.length>0&&definitions.every(card=>card.hidden);});}
function scrollInferenceBottom(){requestAnimationFrame(()=>{const error=inference.querySelector('.error-card');if(error)error.scrollIntoView({block:'end',behavior:'smooth'});else inference.scrollTop=inference.scrollHeight;});}
async function infer(){const tab=activeTab();if(!tab)return;rememberEditor();const currentOpen=output.querySelector('details.definition-card[open]');const currentUid=currentOpen?currentOpen.dataset.uid:'';const serial=++inferenceSerial;try{const body=new URLSearchParams();body.set('file',tab.path);body.set('source',tab.source);appendBuffers(body);const response=await fetch('/run',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body});const html=await response.text();if(serial!==inferenceSerial)return;output.innerHTML=html;let restored=null;if(currentUid){restored=output.querySelector('details.definition-card[data-uid=\"'+CSS.escape(currentUid)+'\"]');if(restored){restored.open=true;loadDefinitionPreview(restored);}}output.querySelectorAll('details.definition-card').forEach(card=>{const local=card.dataset.local==='1';if(local||card.querySelector('.error-card')){card.open=true;const universe=card.closest('details.universe');if(universe)universe.open=true;if(local)loadDefinitionPreview(card);}});restoreRunInputDefaults();applyPanelFilter();scrollInferenceBottom();}catch(error){if(serial!==inferenceSerial)return;output.innerHTML='<div class="error-card"><strong>server error</strong><span>'+esc(error)+'</span></div>';scrollInferenceBottom();}}
async function loadDefinitionPreview(card){const box=card.querySelector('[data-lazy-definition]');if(!box||box.dataset.loaded==='1'||box.dataset.loading==='1')return;const tab=activeTab();if(!tab)return;box.dataset.loading='1';box.innerHTML='<span class="muted">Loading preview…</span>';try{rememberEditor();const body=new URLSearchParams();body.set('file',tab.path);body.set('source',tab.source);body.set('uid',card.dataset.uid);appendBuffers(body);const response=await fetch('/definition',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body});const html=await response.text();if(!response.ok){box.innerHTML=html;box.dataset.loaded='1';card.open=true;const universe=card.closest('details.universe');if(universe)universe.open=true;requestAnimationFrame(()=>{const error=box.querySelector('.error-card');if(error)error.scrollIntoView({block:'nearest'});});return;}box.innerHTML=html;box.dataset.loaded='1';}catch(error){box.innerHTML='<div class="error-card"><strong>preview error</strong><pre class="error-message">'+esc(error)+'</pre></div>';card.open=true;const universe=card.closest('details.universe');if(universe)universe.open=true;}finally{delete box.dataset.loading;}}
output.addEventListener('toggle',event=>{const card=event.target.closest&&event.target.closest('details.definition-card');if(card&&card.open)loadDefinitionPreview(card);},true);
async function loadPickerDirectory(path){try{const response=await fetch('/files?path='+encodeURIComponent(path||''));const data=await response.json();if(data.error)throw new Error(data.error);pickerDirectory=data.path||'';pickerPath.textContent=pickerDirectory||'.';fileUp.disabled=!pickerDirectory;fileList.innerHTML='';for(const entry of data.entries){const button=document.createElement('button');button.className='file-entry'+(entry.directory?' directory':'');button.innerHTML='<span class="kind">'+(entry.directory?'▸':'·')+'</span><span>'+esc(entry.name)+'</span>';button.addEventListener('click',()=>{if(entry.directory)loadPickerDirectory(entry.path);else{filePicker.hidden=true;openWorkspaceFile(entry.path);}});fileList.appendChild(button);}}catch(error){fileList.innerHTML='<div class="error-card"><strong>file browser</strong><span>'+esc(error)+'</span></div>';}}
function showFilePicker(){const tab=activeTab();const slash=tab?tab.path.lastIndexOf('/'):-1;filePicker.hidden=false;loadPickerDirectory(slash>=0?tab.path.slice(0,slash):'');}
editor.addEventListener('input',()=>{const tab=activeTab();if(!tab)return;tab.source=editor.value;tab.dirty=true;highlightEditor();renderTabs();clearTimeout(timer);timer=setTimeout(infer,300);});
editor.addEventListener('scroll',()=>{highlight.scrollTop=editor.scrollTop;highlight.scrollLeft=editor.scrollLeft;const tab=activeTab();if(tab){tab.scrollTop=editor.scrollTop;tab.scrollLeft=editor.scrollLeft;}});
function selectedLineRange(){
    const start=editor.selectionStart,end=editor.selectionEnd,text=editor.value;
    const lineStart=text.lastIndexOf('\n',Math.max(0,start-1))+1;
    let effectiveEnd=end;
    if(end>start&&end>0&&text[end-1]==='\n')effectiveEnd=end-1;
    const nextBreak=text.indexOf('\n',effectiveEnd);
    const lineEnd=nextBreak<0?text.length:nextBreak;
    return{start,end,lineStart,lineEnd};
}
function replaceSelectedLines(transform){
    const r=selectedLineRange();
    const block=editor.value.slice(r.lineStart,r.lineEnd);
    const lines=block.split('\n');
    const changed=transform(lines);
    if(!changed)return;
    const replacement=changed.lines.join('\n');
    editor.setRangeText(replacement,r.lineStart,r.lineEnd,'preserve');
    const oldStart=r.start,oldEnd=r.end;
    const startDelta=changed.startDelta??0,endDelta=changed.endDelta??0;
    editor.setSelectionRange(Math.max(r.lineStart,oldStart+startDelta),Math.max(r.lineStart,oldEnd+endDelta));
    editor.dispatchEvent(new Event('input'));
}
function indentSelection(outdent){
    const r=selectedLineRange();
    if(r.start===r.end&&!outdent){
        editor.setRangeText('    ',r.start,r.end,'end');
        editor.dispatchEvent(new Event('input'));
        return;
    }
    const block=editor.value.slice(r.lineStart,r.lineEnd);
    const lines=block.split('\n');
    let firstDelta=0,totalDelta=0;
    const changed=lines.map((line,index)=>{
        if(!outdent){
            totalDelta+=4;
            if(index===0)firstDelta=4;
            return'    '+line;
        }
        let n=0;
        while(n<4&&n<line.length&&line[n]===' ')n++;
        if(n===0&&line[0]==='\t')n=1;
        totalDelta-=n;
        if(index===0)firstDelta=-n;
        return line.slice(n);
    });
    editor.setRangeText(changed.join('\n'),r.lineStart,r.lineEnd,'preserve');
    const newStart=Math.max(r.lineStart,r.start+firstDelta);
    const newEnd=Math.max(newStart,r.end+totalDelta);
    editor.setSelectionRange(newStart,newEnd);
    editor.dispatchEvent(new Event('input'));
}
function setLineComments(remove){
    const r=selectedLineRange();
    const block=editor.value.slice(r.lineStart,r.lineEnd);
    const lines=block.split('\n');
    let firstDelta=0,totalDelta=0;
    const changed=lines.map((line,index)=>{
        if(remove){
            const n=line.startsWith('//')?2:0;
            totalDelta-=n;if(index===0)firstDelta=-n;return line.slice(n);
        }
        if(line.startsWith('//'))return line;
        totalDelta+=2;if(index===0)firstDelta=2;return'//'+line;
    });
    editor.setRangeText(changed.join('\n'),r.lineStart,r.lineEnd,'preserve');
    const newStart=Math.max(r.lineStart,r.start+firstDelta);
    const newEnd=Math.max(newStart,r.end+totalDelta);
    editor.setSelectionRange(newStart,newEnd);
    editor.dispatchEvent(new Event('input'));
}
function toggleLineComments(){
    const r=selectedLineRange();
    const block=editor.value.slice(r.lineStart,r.lineEnd);
    const lines=block.split('\n');
    const remove=lines.every(line=>line.startsWith('//'));
    let firstDelta=0,totalDelta=0;
    const changed=lines.map((line,index)=>{
        if(remove){
            const n=line.startsWith('//')?2:0;
            totalDelta-=n;
            if(index===0)firstDelta=-n;
            return line.slice(n);
        }
        totalDelta+=2;
        if(index===0)firstDelta=2;
        return'//'+line;
    });
    editor.setRangeText(changed.join('\n'),r.lineStart,r.lineEnd,'preserve');
    const newStart=Math.max(r.lineStart,r.start+firstDelta);
    const newEnd=Math.max(newStart,r.end+totalDelta);
    editor.setSelectionRange(newStart,newEnd);
    editor.dispatchEvent(new Event('input'));
}
function controlledWordBackspace(){
    const start=editor.selectionStart,end=editor.selectionEnd;
    if(start!==end){
        editor.setRangeText('',start,end,'end');
        editor.dispatchEvent(new Event('input'));
        return;
    }
    const text=editor.value;
    const lineStart=text.lastIndexOf('\n',Math.max(0,start-1))+1;
    if(start<=lineStart)return;
    let cut=start;
    if(/[ \t]/.test(text[cut-1])){
        while(cut>lineStart&&/[ \t]/.test(text[cut-1]))cut--;
    }else{
        while(cut>lineStart&&!/[ \t]/.test(text[cut-1]))cut--;
    }
    editor.setRangeText('',cut,start,'end');
    editor.dispatchEvent(new Event('input'));
}
function editorSearchMatches(){const q=editorSearchInput.value;if(!q)return[];const text=editor.value.toLowerCase(),needle=q.toLowerCase(),out=[];let at=0;while((at=text.indexOf(needle,at))!==-1){out.push(at);at+=Math.max(1,needle.length);}return out;}
function updateEditorSearchCount(){const matches=editorSearchMatches();if(!matches.length){editorSearchCount.textContent=editorSearchInput.value?'0/0':'';return;}const pos=editor.selectionStart;let index=matches.findIndex(at=>at>=pos);if(index<0)index=matches.length-1;editorSearchCount.textContent=(index+1)+'/'+matches.length;}
function openEditorSearch(){editorSearch.hidden=false;const start=editor.selectionStart,end=editor.selectionEnd;if(end>start&&!editor.value.slice(start,end).includes('\n'))editorSearchInput.value=editor.value.slice(start,end);editorSearchInput.focus();editorSearchInput.select();updateEditorSearchCount();}
function closeEditorSearch(){editorSearch.hidden=true;editor.focus();}
function moveEditorSearch(direction){const matches=editorSearchMatches();if(!matches.length){updateEditorSearchCount();return;}const pos=editor.selectionStart;let index;if(direction>0){index=matches.findIndex(at=>at>pos);if(index<0)index=0;}else{index=-1;for(let i=matches.length-1;i>=0;i--)if(matches[i]<pos){index=i;break;}if(index<0)index=matches.length-1;}const at=matches[index];editor.focus();editor.setSelectionRange(at,at+editorSearchInput.value.length);const before=editor.value.slice(0,at);const line=before.split('\n').length;const lineHeight=parseFloat(getComputedStyle(editor).lineHeight)||23;editor.scrollTop=Math.max(0,(line-3)*lineHeight);highlight.scrollTop=editor.scrollTop;editorSearchCount.textContent=(index+1)+'/'+matches.length;}
editorSearchInput.addEventListener('input',()=>{updateEditorSearchCount();});editorSearchInput.addEventListener('keydown',event=>{if(event.key==='Enter'){event.preventDefault();moveEditorSearch(event.shiftKey?-1:1);}else if(event.key==='Escape'){event.preventDefault();closeEditorSearch();}});document.getElementById('editorSearchPrev').addEventListener('click',()=>moveEditorSearch(-1));document.getElementById('editorSearchNext').addEventListener('click',()=>moveEditorSearch(1));document.getElementById('editorSearchClose').addEventListener('click',closeEditorSearch);
function closeMenus(){document.querySelectorAll('.menu.open').forEach(menu=>menu.classList.remove('open'));}
document.querySelectorAll('.menu-button[data-menu]').forEach(button=>button.addEventListener('click',event=>{event.stopPropagation();const menu=document.getElementById(button.dataset.menu);const opening=!menu.classList.contains('open');closeMenus();if(opening)menu.classList.add('open');}));
document.addEventListener('click',event=>{if(!event.target.closest('.menu'))closeMenus();});
function editorHistory(command){editor.focus();document.execCommand(command);editor.dispatchEvent(new Event('input'));}
document.getElementById('editUndo').addEventListener('click',()=>{closeMenus();editorHistory('undo');});
document.getElementById('editRedo').addEventListener('click',()=>{closeMenus();editorHistory('redo');});
document.getElementById('editSearch').addEventListener('click',()=>{closeMenus();openEditorSearch();});
document.getElementById('editComment').addEventListener('click',()=>{closeMenus();setLineComments(false);});
document.getElementById('editUncomment').addEventListener('click',()=>{closeMenus();setLineComments(true);});
document.getElementById('editTabIn').addEventListener('click',()=>{closeMenus();indentSelection(false);});
document.getElementById('editTabOut').addEventListener('click',()=>{closeMenus();indentSelection(true);});
editor.addEventListener('keydown',event=>{
    const command=event.ctrlKey||event.metaKey;
    if(event.key==='Tab'){
        event.preventDefault();
        indentSelection(event.shiftKey);
        return;
    }
    if(command&&event.key==='Backspace'){
        event.preventDefault();
        controlledWordBackspace();
        return;
    }
    if(command&&event.key==='/'){
        event.preventDefault();
        if(event.shiftKey)setLineComments(true);else toggleLineComments();
        return;
    }
    if(command&&event.key.toLowerCase()==='f'){
        event.preventDefault();
        openEditorSearch();
        return;
    }
    if(command&&event.key==='Enter'){
        event.preventDefault();
        infer();
        return;
    }
    if(command&&event.key.toLowerCase()==='s'){
        event.preventDefault();
        if(event.altKey)saveEveryTab();else if(event.shiftKey)saveAsActive();else saveActive();
        return;
    }
    if(command&&event.key.toLowerCase()==='n'){event.preventDefault();newFile();return;}
    if(command&&event.key.toLowerCase()==='o'){
        event.preventDefault();
        showFilePicker();
    }
});
newFileButton.addEventListener('click',()=>{closeMenus();newFile();});openFileButton.addEventListener('click',()=>{closeMenus();showFilePicker();});saveFileButton.addEventListener('click',()=>{closeMenus();saveActive();});saveAsButton.addEventListener('click',()=>{closeMenus();saveAsActive();});saveAllButton.addEventListener('click',()=>{closeMenus();saveEveryTab();});closePicker.addEventListener('click',()=>filePicker.hidden=true);filePicker.addEventListener('click',event=>{if(event.target===filePicker)filePicker.hidden=true;});fileUp.addEventListener('click',()=>{if(!pickerDirectory)return;const slash=pickerDirectory.lastIndexOf('/');loadPickerDirectory(slash<0?'':pickerDirectory.slice(0,slash));});panelSearch.addEventListener('input',applyPanelFilter);
document.addEventListener('input',event=>{const input=event.target.closest&&event.target.closest('.run-card input[data-name]');if(input)cacheRunInput(input);});
document.addEventListener('click',async event=>{const sourceButton=event.target.closest('.source-open');if(sourceButton){await openWorkspaceFile(sourceButton.dataset.file,Number(sourceButton.dataset.line||0));return;}const button=event.target.closest('.run-exec');if(!button)return;const tab=activeTab();if(!tab)return;rememberEditor();const card=button.closest('.run-card');const result=card.querySelector('.run-results');const body=new URLSearchParams();body.set('file',tab.path);body.set('source',tab.source);body.set('run',card.dataset.run);appendBuffers(body);card.querySelectorAll('input[data-name]').forEach(input=>{cacheRunInput(input);body.append('value.'+input.dataset.name,input.value);});button.disabled=true;try{const response=await fetch('/execute',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body});const data=await response.json();if(data.error){let location='';if(data.error_file){location='<button class="source-open error-location" data-file="'+esc(data.error_file)+'" data-line="'+Number(data.error_line||0)+'">'+esc(data.error_file+':'+(data.error_line||0)+':'+(data.error_column||0))+'</button>';}result.innerHTML='<div class="error-card"><strong>run error</strong><pre class="error-message">'+esc(data.error)+'</pre>'+location+'</div>';}else{result.innerHTML=data.results.map(item=>'<div class="statement"><span class="name">'+esc(item.name)+'</span> <span class="muted">=</span> '+esc(item.value)+'</div>').join('');}requestAnimationFrame(()=>requestAnimationFrame(()=>result.scrollIntoView({block:'end',behavior:'smooth'})));}catch(error){result.innerHTML='<div class="error-card"><strong>server error</strong><pre class="error-message">'+esc(error)+'</pre></div>';requestAnimationFrame(()=>result.scrollIntoView({block:'end',behavior:'smooth'}));}finally{button.disabled=false;}});
const initialFile=editor.dataset.initialFile;
if(initialFile){tabs.push({path:initialFile,source:editor.value,dirty:false,scrollTop:0,scrollLeft:0,selectionStart:0,selectionEnd:0});activeIndex=0;editor.disabled=false;renderTabs();highlightEditor();infer();}else{editor.value='';editor.disabled=true;renderTabs();highlightEditor();showFilePicker();}
</script>
<script type="module">
import Graphology from 'https://cdn.jsdelivr.net/npm/graphology@0.26.0/+esm';
import Sigma from 'https://cdn.jsdelivr.net/npm/sigma@3.0.2/+esm';

const graphModal=document.getElementById('graphModal');
const graphGrid=document.getElementById('graphGrid');
const graphClose=document.getElementById('graphClose');
let graphRenderers=[];

function destroyGraphs(){
    for(const renderer of graphRenderers){
        try{renderer.kill();}catch(_){}
    }
    graphRenderers=[];
    graphGrid.replaceChildren();
}
function closeGraph(){
    destroyGraphs();
    graphModal.hidden=true;
}
function nodeInfo(node){
    const aliases=[...new Set((node.aliases||[]).filter(Boolean))];
    return aliases.join('\n');
}
function makeGraphPane(pane){
    const section=document.createElement('section');
    section.className='graph-pane';
    const title=document.createElement('div');
    title.className='graph-pane-title';
    title.textContent=pane.title||'Graph';
    const wrap=document.createElement('div');
    wrap.className='sigma-wrap';
    const host=document.createElement('div');
    host.className='sigma-host';
    const overlay=document.createElement('canvas');
    overlay.className='edge-overlay';
    const labels=document.createElement('div');
    labels.className='node-label-overlay';
    const tip=document.createElement('div');
    tip.className='graph-tip';
    wrap.append(host,overlay,labels,tip);
    section.append(title,wrap);
    graphGrid.appendChild(section);

    const data=pane.graph||{nodes:[],edges:[]};
    const graph=new Graphology({type:'directed',multi:true});
    const count=Math.max(1,data.nodes.length);
    data.nodes.forEach((node,index)=>{
        const angle=index*2*Math.PI/count;
        graph.addNode(node.key,{x:Math.cos(angle),y:Math.sin(angle),size:13,color:'#334155',label:''});
    });
    data.edges.forEach((edge,index)=>{
        if(graph.hasNode(edge.source)&&graph.hasNode(edge.target)){
            const visualKey=String(edge.key??'edge')+'#'+index;
            graph.addEdgeWithKey(visualKey,edge.source,edge.target,{size:.1,color:'#00000000'});
        }
    });
    const renderer=new Sigma(graph,host,{
        renderEdgeLabels:false,
        defaultEdgeType:'line',
        minCameraRatio:.2,
        maxCameraRatio:5
    });
    graphRenderers.push(renderer);

    function draw(){
        const bounds=overlay.getBoundingClientRect();
        if(!bounds.width||!bounds.height)return;
        const dpr=window.devicePixelRatio||1;
        overlay.width=Math.max(1,Math.round(bounds.width*dpr));
        overlay.height=Math.max(1,Math.round(bounds.height*dpr));
        const ctx=overlay.getContext('2d');
        ctx.setTransform(dpr,0,0,dpr,0,0);
        ctx.clearRect(0,0,bounds.width,bounds.height);
        ctx.font='12px ui-monospace,SFMono-Regular,Menlo,Consolas,monospace';
        ctx.textAlign='center';
        ctx.textBaseline='bottom';
        for(const edge of data.edges){
            if(!graph.hasNode(edge.source)||!graph.hasNode(edge.target))continue;
            const a=renderer.graphToViewport(graph.getNodeAttributes(edge.source));
            const b=renderer.graphToViewport(graph.getNodeAttributes(edge.target));
            const dx=b.x-a.x,dy=b.y-a.y,len=Math.hypot(dx,dy)||1,ux=dx/len,uy=dy/len;
            const ex=b.x-ux*13,ey=b.y-uy*13;
            ctx.beginPath();
            ctx.setLineDash([7,6]);
            ctx.lineWidth=1.5;
            ctx.strokeStyle='#ff6b78';
            ctx.moveTo(a.x,a.y);
            ctx.lineTo(ex,ey);
            ctx.stroke();
            ctx.setLineDash([]);
            ctx.beginPath();
            ctx.fillStyle='#ff6b78';
            ctx.moveTo(ex,ey);
            ctx.lineTo(ex-ux*9-uy*5,ey-uy*9+ux*5);
            ctx.lineTo(ex-ux*9+uy*5,ey-uy*9-ux*5);
            ctx.closePath();
            ctx.fill();
            const mx=(a.x+ex)/2,my=(a.y+ey)/2;
            ctx.lineWidth=4;
            ctx.strokeStyle='#11151d';
            ctx.strokeText(edge.label,mx,my-3);
            ctx.fillStyle='#ff6b78';
            ctx.fillText(edge.label,mx,my-3);
        }
        labels.replaceChildren();
        for(const node of data.nodes){
            if(!graph.hasNode(node.key))continue;
            const p=renderer.graphToViewport(graph.getNodeAttributes(node.key));
            const label=document.createElement('div');
            label.className='node-label';
            label.textContent=node.label;
            label.style.left=p.x+'px';
            label.style.top=p.y+'px';
            labels.appendChild(label);
        }
    }
    renderer.on('afterRender',draw);
    renderer.on('enterNode',event=>{
        const node=data.nodes.find(item=>item.key===event.node);
        if(!node)return;
        tip.textContent=nodeInfo(node);
        tip.classList.add('open');
        const p=renderer.graphToViewport(graph.getNodeAttributes(event.node));
        requestAnimationFrame(()=>{
            const r=wrap.getBoundingClientRect();
            tip.style.left=Math.max(8,Math.min(r.width-tip.offsetWidth-8,p.x+16))+'px';
            tip.style.top=Math.max(8,Math.min(r.height-tip.offsetHeight-8,p.y+16))+'px';
        });
    });
    renderer.on('leaveNode',()=>tip.classList.remove('open'));
    requestAnimationFrame(()=>requestAnimationFrame(draw));
}
function openGraph(payload){
    destroyGraphs();
    const panes=Array.isArray(payload?.panes)?payload.panes:[];
    if(!panes.length)return;
    graphGrid.classList.toggle('split',payload.mode==='split'&&panes.length===2);
    graphModal.hidden=false;
    requestAnimationFrame(()=>requestAnimationFrame(()=>panes.forEach(makeGraphPane)));
}
document.addEventListener('click',async event=>{
    const button=event.target.closest('.graph-btn');
    if(!button)return;
    event.preventDefault();
    event.stopPropagation();
    try{
        if(button.dataset.definitionUid){const tab=activeTab();if(!tab)return;rememberEditor();const body=new URLSearchParams();body.set('file',tab.path);body.set('source',tab.source);body.set('uid',button.dataset.definitionUid);appendBuffers(body);button.disabled=true;const response=await fetch('/definition-graph',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body});const text=await response.text();if(!response.ok)throw new Error(text);openGraph(JSON.parse(text));button.disabled=false;return;}
        openGraph(JSON.parse(button.dataset.graph));
    }catch(error){button.disabled=false;console.error('graph preview failed',error);}
});
graphClose.addEventListener('click',closeGraph);
graphModal.addEventListener('click',event=>{if(event.target===graphModal)closeGraph();});
document.addEventListener('keydown',event=>{if(event.key==='Escape'&&!graphModal.hidden)closeGraph();});
</script>
</body>
</html>)HTML";
        return page.str();
    }

    static int serve_http(const std::string& initial_source, const std::string& file) {
        httplib::Server server;
        const std::filesystem::path workspace_root = std::filesystem::weakly_canonical(std::filesystem::current_path());
        std::string initial_file;
        if (!file.empty()) {
            auto relative = workspace_relative_path(workspace_root, file);
            if (!relative) {
                throw std::runtime_error("--serve FILE must be inside the current working directory or one of its subdirectories");
            }
            initial_file = *relative;
            require_gs_editor_path(workspace_path(workspace_root, initial_file, true));
        }
        const std::string page = playground_document(initial_source, initial_file);

        server.Get("/", [&](const httplib::Request&, httplib::Response& response) {
            response.set_content(page, "text/html; charset=utf-8");
        });

        server.Get("/files", [&](const httplib::Request& request, httplib::Response& response) {
            try {
                std::string path = request.has_param("path") ? request.get_param_value("path") : std::string{};
                response.set_content(workspace_listing_json(workspace_root, path), "application/json; charset=utf-8");
            } catch (const std::exception& error) {
                response.status = 400;
                response.set_content("{\"error\":" + json_escape(error.what()) + "}", "application/json; charset=utf-8");
            }
        });

        server.Get("/file", [&](const httplib::Request& request, httplib::Response& response) {
            try {
                if (!request.has_param("path")) throw std::runtime_error("missing file path");
                response.set_content(workspace_file_json(workspace_root, request.get_param_value("path")), "application/json; charset=utf-8");
            } catch (const std::exception& error) {
                response.status = 400;
                response.set_content("{\"error\":" + json_escape(error.what()) + "}", "application/json; charset=utf-8");
            }
        });

        server.Post("/save", [&](const httplib::Request& request, httplib::Response& response) {
            try {
                if (!request.has_param("path")) throw std::runtime_error("missing file path");
                std::string source = request.has_param("source") ? request.get_param_value("source") : std::string{};
                save_workspace_file(workspace_root, request.get_param_value("path"), source);
                response.set_content("{\"ok\":true}", "application/json; charset=utf-8");
            } catch (const std::exception& error) {
                response.status = 400;
                response.set_content("{\"error\":" + json_escape(error.what()) + "}", "application/json; charset=utf-8");
            }
        });

        server.Post("/run", [&](const httplib::Request& request, httplib::Response& response) {
            try {
                if (!request.has_param("file")) throw std::runtime_error("missing active file");
                std::filesystem::path active = workspace_path(workspace_root, request.get_param_value("file"), false);
                require_gs_editor_path(active);
                std::string source = request.has_param("source") ? request.get_param_value("source") : std::string{};
                EditorBuffers buffers = editor_buffers_from_request(request);
                response.set_content(
                    process_editor_source(source, active.string(), buffers, workspace_root),
                    "text/html; charset=utf-8"
                );
            } catch (const std::exception& error) {
                response.set_content(html_exception(error.what()), "text/html; charset=utf-8");
            }
        });

        auto definition_request = [&](const httplib::Request& request, httplib::Response& response, bool graph_only) {
            try {
                if (!request.has_param("file")) throw std::runtime_error("missing active file");
                if (!request.has_param("uid")) throw std::runtime_error("missing definition uid");
                std::filesystem::path active = workspace_path(workspace_root, request.get_param_value("file"), false);
                require_gs_editor_path(active);
                std::string source = request.has_param("source") ? request.get_param_value("source") : std::string{};
                size_t parsed = 0;
                std::string raw_uid = request.get_param_value("uid");
                uint64_t uid = std::stoull(raw_uid, &parsed);
                if (parsed != raw_uid.size()) throw std::runtime_error("invalid definition uid");
                EditorBuffers buffers = editor_buffers_from_request(request);
                response.set_content(editor_definition_data(source, active.string(), uid, graph_only, buffers, workspace_root),
                    graph_only ? "application/json; charset=utf-8" : "text/html; charset=utf-8");
            } catch (const Error& error) {
                response.status = 400;
                response.set_content(graph_only ? ("{\"error\":" + json_escape(error.what()) + "}") : html_exception(error.what()),
                    graph_only ? "application/json; charset=utf-8" : "text/html; charset=utf-8");
            } catch (const std::exception& error) {
                response.status = 400;
                response.set_content(graph_only ? ("{\"error\":" + json_escape(error.what()) + "}") : html_exception(error.what()),
                    graph_only ? "application/json; charset=utf-8" : "text/html; charset=utf-8");
            }
        };
        server.Post("/definition", [&](const httplib::Request& request, httplib::Response& response) {
            definition_request(request, response, false);
        });
        server.Post("/definition-graph", [&](const httplib::Request& request, httplib::Response& response) {
            definition_request(request, response, true);
        });

        server.Post("/execute", [&](const httplib::Request& request, httplib::Response& response) {
            try {
                if (!request.has_param("file")) throw std::runtime_error("missing active file");
                std::filesystem::path active = workspace_path(workspace_root, request.get_param_value("file"), false);
                require_gs_editor_path(active);
                std::string source = request.has_param("source") ? request.get_param_value("source") : std::string{};
                size_t run_index = 0;
                if (!request.has_param("run")) throw std::runtime_error("missing run request");
                size_t parsed = 0;
                std::string raw_run = request.get_param_value("run");
                run_index = std::stoull(raw_run, &parsed);
                if (parsed != raw_run.size()) throw std::runtime_error("invalid run request");

                std::unordered_map<std::string, std::string> values;
                for (const auto& [key, value] : request.params) {
                    if (key.rfind("value.", 0) == 0) values[key.substr(6)] = value;
                }
                EditorBuffers buffers = editor_buffers_from_request(request);
                response.set_content(
                    execute_editor_run(source, active.string(), run_index, values, buffers, workspace_root),
                    "application/json; charset=utf-8"
                );
            } catch (const std::exception& error) {
                response.set_content("{\"error\":" + json_escape(error.what()) + "}", "application/json; charset=utf-8");
            }
        });

        server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
            if (response.status == 404) response.set_content("not found", "text/plain; charset=utf-8");
        });

        int port = server.bind_to_any_port("127.0.0.1");
        if (port < 0) throw std::runtime_error("could not bind HTTP server to 127.0.0.1");
        std::cout << "serving http://127.0.0.1:" << port << "/\n" << std::flush;
        if (!server.listen_after_bind()) throw std::runtime_error("HTTP server stopped unexpectedly");
        return 0;
    }

    static std::string pretty_error(const Error&e,const std::unordered_map<std::string,std::string>&sources){
        std::ostringstream out;
        out<<ansi::BOLD<<ansi::RED<<"error:"<<ansi::RESET<<" "<<ansi::RED<<e.what()<<ansi::RESET<<"\n"<<" "<<ansi::PURPLE<<"-->"<<ansi::RESET<<" "<<e.file<<":"<<e.span.start.line<<":"<<e.span.start.column;
        auto it=sources.find(e.file);
        if(it!=sources.end()){
            std::istringstream in(it->second);
            std::string line;
            uint32_t ln=1;
            while(ln<e.span.start.line&&std::getline(in,line))++ln;
            if(ln==e.span.start.line&&std::getline(in,line)){
                size_t a=e.span.start.column?e.span.start.column-1:0;
                size_t n=(e.span.end.line==e.span.start.line&&e.span.end.column>e.span.start.column)?e.span.end.column-e.span.start.column:1;
                size_t b=std::min(line.size(),a+n);
                out<<"\n "<<ansi::PURPLE<<"|"<<ansi::RESET<<"\n "<<e.span.start.line<<" "<<ansi::PURPLE<<"|"<<ansi::RESET<<" ";
                if(a<=line.size())out<<line.substr(0,a)<<ansi::RED_UL<<line.substr(a,b-a)<<ansi::RESET<<line.substr(b);
                else out<<line;
                out<<"\n "<<ansi::PURPLE<<"|"<<ansi::RESET<<" "<<std::string(a,' ')<<ansi::RED<<std::string(std::max<size_t>(1,b-a),'^')<<ansi::RESET;
        }}if(!e.graph_context.empty())out<<"\n "<<ansi::PURPLE<<"|"<<ansi::RESET<<"\n"<<ansi::BOLD<<" graph context:"<<ansi::RESET<<"\n"<<e.graph_context;
        return out.str();
    }

} // namespace gras

int main(int argc, char** argv) {
    using namespace gras;

    enum class Mode {
        None,
        Cli,
        Serve
    };

    Mode mode = Mode::None;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--cli") {
            if (mode != Mode::None) {
                std::cerr << "error: --cli and --serve are mutually exclusive\n";
                return 2;
            }
            mode = Mode::Cli;
        } else if (arg == "--serve") {
            if (mode != Mode::None) {
                std::cerr << "error: --cli and --serve are mutually exclusive\n";
                return 2;
            }
            mode = Mode::Serve;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: " << argv[0] << " --cli FILE\n"
                      << "       " << argv[0] << " --serve [FILE]\n";
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option '" << arg << "'\n";
            return 2;
        } else {
            positional.push_back(std::move(arg));
        }
    }

    if (mode == Mode::None || positional.size() > 1 || (mode == Mode::Cli && positional.empty())) {
        std::cerr << "usage: " << argv[0] << " --cli FILE\n"
                  << "       " << argv[0] << " --serve [FILE]\n";
        return 2;
    }

    if (mode == Mode::Serve) {
        try {
            std::string initial_source;
            std::string file;
            if (!positional.empty()) {
                std::filesystem::path path = std::filesystem::absolute(positional.front()).lexically_normal();
                file = path.string();
                initial_source = read_file(path);
            }
            return serve_http(initial_source, file);
        } catch (const std::exception& error) {
            std::cerr << "error: " << error.what() << "\n";
            return 1;
        }
    }

    Interner names;
    Loader loader(names);
    std::string file = std::filesystem::absolute(positional.front()).lexically_normal().string();
    Program program;

    try {
        program = loader.load(file);
    } catch (const Error& error) {
        if (loader.last_partial_program) {
            try {
                Program partial = *loader.last_partial_program;
                Resolver(file, partial, names, loader.next_uid).resolve();
                TypeRegistry registry(names);
                print_program(partial, file, names, registry);
            } catch (...) {
            }
        }
        std::cerr << pretty_error(error, loader.sources) << "\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << ansi::BOLD << ansi::RED << "error:" << ansi::RESET << " " << error.what() << "\n";
        return 1;
    }

    Program unresolved = program;
    try {
        Resolver(file, program, names, loader.next_uid).resolve();
    } catch (const Error& error) {
        try {
            Program partial = program_before_error(unresolved, error);
            Resolver(file, partial, names, loader.next_uid).resolve();
            TypeRegistry registry(names);
            print_program(partial, file, names, registry);
        } catch (...) {
        }
        std::cerr << pretty_error(error, loader.sources) << "\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << ansi::BOLD << ansi::RED << "error:" << ansi::RESET << " " << error.what() << "\n";
        return 1;
    }

    try {
        TypeRegistry registry(names);
        print_program(program, file, names, registry);
        console_runs(file, program, names, registry);
        return 0;
    } catch (const Error& error) {
        std::cerr << pretty_error(error, loader.sources) << "\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << ansi::BOLD << ansi::RED << "error:" << ansi::RESET << " " << error.what() << "\n";
        return 1;
    }
}
