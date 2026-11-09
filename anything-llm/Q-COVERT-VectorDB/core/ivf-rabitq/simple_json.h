#ifndef IVF_RABITQ_SIMPLE_JSON_H
#define IVF_RABITQ_SIMPLE_JSON_H

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace simple_json {

struct Value {
    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Object,
        Array
    };

    Type type = Type::Null;
    bool bool_value = false;
    double number_value = 0.0;
    std::string string_value;
    std::map<std::string, Value> object_value;
    std::vector<Value> array_value;

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }

    const Value* get(const std::string& key) const {
        if (!is_object()) {
            return nullptr;
        }
        const auto it = object_value.find(key);
        return it == object_value.end() ? nullptr : &it->second;
    }
};

class Parser {
public:
    explicit Parser(std::string text) : text_(std::move(text)) {}

    Value parse() {
        skip_ws();
        Value value = parse_value();
        skip_ws();
        if (pos_ != text_.size()) {
            fail("unexpected trailing characters");
        }
        return value;
    }

private:
    std::string text_;
    std::size_t pos_ = 0;

    static void append_utf8(std::string& output, uint32_t codepoint) {
        if (codepoint <= 0x7f) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0xffff) {
            output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0x10ffff) {
            output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else {
            throw std::runtime_error("JSON unicode codepoint is out of range");
        }
    }

    static bool valid_utf8(const std::string& input) {
        size_t i = 0;
        while (i < input.size()) {
            const uint8_t first = static_cast<uint8_t>(input[i++]);
            if (first <= 0x7f) continue;
            uint32_t codepoint = 0;
            size_t continuation = 0;
            uint32_t minimum = 0;
            if ((first & 0xe0U) == 0xc0U) { codepoint = first & 0x1fU; continuation = 1; minimum = 0x80; }
            else if ((first & 0xf0U) == 0xe0U) { codepoint = first & 0x0fU; continuation = 2; minimum = 0x800; }
            else if ((first & 0xf8U) == 0xf0U) { codepoint = first & 0x07U; continuation = 3; minimum = 0x10000; }
            else return false;
            if (i + continuation > input.size()) return false;
            for (size_t j = 0; j < continuation; ++j) {
                const uint8_t byte = static_cast<uint8_t>(input[i++]);
                if ((byte & 0xc0U) != 0x80U) return false;
                codepoint = (codepoint << 6U) | (byte & 0x3fU);
            }
            if (codepoint < minimum || codepoint > 0x10ffff ||
                (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
        }
        return true;
    }

    uint32_t parse_hex_quad() {
        uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ >= text_.size()) fail("short unicode escape");
            const char hex = take();
            code <<= 4U;
            if (hex >= '0' && hex <= '9') code += static_cast<uint32_t>(hex - '0');
            else if (hex >= 'a' && hex <= 'f') code += static_cast<uint32_t>(hex - 'a' + 10);
            else if (hex >= 'A' && hex <= 'F') code += static_cast<uint32_t>(hex - 'A' + 10);
            else fail("invalid unicode escape");
        }
        return code;
    }

    [[noreturn]] void fail(const std::string& message) const {
        std::ostringstream oss;
        oss << "JSON parse error at byte " << pos_ << ": " << message;
        throw std::runtime_error(oss.str());
    }

    void skip_ws() {
        while (pos_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            pos_++;
        }
    }

    char peek() const {
        return pos_ < text_.size() ? text_[pos_] : '\0';
    }

    char take() {
        if (pos_ >= text_.size()) {
            fail("unexpected end of input");
        }
        return text_[pos_++];
    }

    void expect(char expected) {
        const char actual = take();
        if (actual != expected) {
            std::ostringstream oss;
            oss << "expected '" << expected << "', got '" << actual << "'";
            fail(oss.str());
        }
    }

    Value parse_value() {
        skip_ws();
        const char ch = peek();
        if (ch == '{') {
            return parse_object();
        }
        if (ch == '[') {
            return parse_array();
        }
        if (ch == '"') {
            Value value;
            value.type = Value::Type::String;
            value.string_value = parse_string();
            return value;
        }
        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) {
            return parse_number();
        }
        if (starts_with("true")) {
            pos_ += 4;
            Value value;
            value.type = Value::Type::Bool;
            value.bool_value = true;
            return value;
        }
        if (starts_with("false")) {
            pos_ += 5;
            Value value;
            value.type = Value::Type::Bool;
            value.bool_value = false;
            return value;
        }
        if (starts_with("null")) {
            pos_ += 4;
            return Value{};
        }
        fail("expected JSON value");
    }

    bool starts_with(const char* literal) const {
        std::size_t i = 0;
        while (literal[i] != '\0') {
            if (pos_ + i >= text_.size() || text_[pos_ + i] != literal[i]) {
                return false;
            }
            i++;
        }
        return true;
    }

    Value parse_object() {
        Value value;
        value.type = Value::Type::Object;
        expect('{');
        skip_ws();
        if (peek() == '}') {
            take();
            return value;
        }
        while (true) {
            skip_ws();
            if (peek() != '"') {
                fail("expected object key string");
            }
            std::string key = parse_string();
            skip_ws();
            expect(':');
            value.object_value.emplace(std::move(key), parse_value());
            skip_ws();
            const char ch = take();
            if (ch == '}') {
                return value;
            }
            if (ch != ',') {
                fail("expected ',' or '}' in object");
            }
        }
    }

    Value parse_array() {
        Value value;
        value.type = Value::Type::Array;
        expect('[');
        skip_ws();
        if (peek() == ']') {
            take();
            return value;
        }
        while (true) {
            value.array_value.push_back(parse_value());
            skip_ws();
            const char ch = take();
            if (ch == ']') {
                return value;
            }
            if (ch != ',') {
                fail("expected ',' or ']' in array");
            }
        }
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            if (pos_ >= text_.size()) {
                fail("unterminated string");
            }
            const char ch = take();
            if (ch == '"') {
                if (!valid_utf8(out)) fail("string is not valid UTF-8");
                return out;
            }
            if (static_cast<unsigned char>(ch) < 0x20) {
                fail("control character in string");
            }
            if (ch != '\\') {
                out.push_back(ch);
                continue;
            }
            const char esc = take();
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t code = parse_hex_quad();
                    if (code >= 0xd800 && code <= 0xdbff) {
                        if (take() != '\\' || take() != 'u') fail("high surrogate lacks low surrogate");
                        const uint32_t low = parse_hex_quad();
                        if (low < 0xdc00 || low > 0xdfff) fail("invalid low surrogate");
                        code = 0x10000U + ((code - 0xd800U) << 10U) + (low - 0xdc00U);
                    } else if (code >= 0xdc00 && code <= 0xdfff) {
                        fail("lone low surrogate");
                    }
                    append_utf8(out, code);
                    break;
                }
                default:
                    fail("invalid string escape");
            }
        }
    }

    Value parse_number() {
        const std::size_t start = pos_;
        if (peek() == '-') {
            pos_++;
        }
        if (peek() == '0') {
            pos_++;
        } else {
            if (!std::isdigit(static_cast<unsigned char>(peek()))) {
                fail("invalid number");
            }
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                pos_++;
            }
        }
        if (peek() == '.') {
            pos_++;
            if (!std::isdigit(static_cast<unsigned char>(peek()))) {
                fail("invalid number fraction");
            }
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                pos_++;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            pos_++;
            if (peek() == '+' || peek() == '-') {
                pos_++;
            }
            if (!std::isdigit(static_cast<unsigned char>(peek()))) {
                fail("invalid number exponent");
            }
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                pos_++;
            }
        }

        errno = 0;
        char* end = nullptr;
        const std::string token = text_.substr(start, pos_ - start);
        const double parsed = std::strtod(token.c_str(), &end);
        if (errno != 0 || end == token.c_str() || *end != '\0') {
            fail("invalid number value");
        }
        Value value;
        value.type = Value::Type::Number;
        value.number_value = parsed;
        return value;
    }
};

inline bool file_exists(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return in.good();
}

inline Value parse_text(const std::string& text) {
    return Parser(text).parse();
}

inline Value parse_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open JSON config: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse_text(ss.str());
}

inline std::string as_string(const Value& value, const std::string& path) {
    if (!value.is_string()) {
        throw std::runtime_error("config value must be a string: " + path);
    }
    return value.string_value;
}

inline double as_number(const Value& value, const std::string& path) {
    if (!value.is_number()) {
        throw std::runtime_error("config value must be a number: " + path);
    }
    return value.number_value;
}

inline bool as_bool(const Value& value, const std::string& path) {
    if (!value.is_bool()) {
        throw std::runtime_error("config value must be a bool: " + path);
    }
    return value.bool_value;
}

inline const Value* get_path(const Value& root, const std::vector<std::string>& keys) {
    const Value* current = &root;
    for (const std::string& key : keys) {
        if (current == nullptr) {
            return nullptr;
        }
        current = current->get(key);
    }
    return current;
}

} // namespace simple_json

#endif // IVF_RABITQ_SIMPLE_JSON_H
