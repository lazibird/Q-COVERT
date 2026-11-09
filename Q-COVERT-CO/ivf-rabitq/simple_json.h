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
                    int code = 0;
                    for (int i = 0; i < 4; i++) {
                        if (pos_ >= text_.size()) {
                            fail("short unicode escape");
                        }
                        const char hex = take();
                        code <<= 4;
                        if (hex >= '0' && hex <= '9') {
                            code += hex - '0';
                        } else if (hex >= 'a' && hex <= 'f') {
                            code += hex - 'a' + 10;
                        } else if (hex >= 'A' && hex <= 'F') {
                            code += hex - 'A' + 10;
                        } else {
                            fail("invalid unicode escape");
                        }
                    }
                    out.push_back(code >= 0 && code < 128 ? static_cast<char>(code) : '?');
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
