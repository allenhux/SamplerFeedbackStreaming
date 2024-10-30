/*=============================================================================
Reads & Writes files of the form:

{
    // c++ style comments
    "name" : value, // name-value pairs, comma-separated
    "block": {      // the "value" can be a series of name-value pairs between  {}
        "name1" : value1,
        "name2" : value2
    },
    "array": [ a, b, c ], // the "value" can be an array of values

    // any value can also be an array or a block
    "a2" : [
                {
                    "x" : false,
                    "y" : 4
                },
                3
           ]
}

Read a file:

    JsonParser parser;
    parser.Read("filename");

Use [] to access a value by name or index, convert type with appropriate "asFoo()"

    auto& root = parser.GetRoot();

    std::string s = root["name"].asString();
    bool x = root["a2"][0]["x"].asBool();
    int z = root["a2"][1].asInt();

Iterator support:

    // parse an array of arrays of floats
    for (const auto& pose : root["Poses"])
    {
        std::vector<float> f;
        for (UINT i = 0; i < pose.size(); i++)
        {
            f.push_back(pose[i].asFloat());
        }
        // now do something with f...
    }

Output:

    // to stdout
    parser.Write(std::cout);

    // to file
    std::ofstream ofs(in_filePath, std::ios::out);
    m_value.Write(ofs);

Known issues:

    reading/writing values that contain quotes, e.g. "v" : "\"value\"";
    only supports one root object { ... }

=============================================================================*/
#pragma once
#include <stdint.h>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <assert.h>
#include <algorithm>
#include <iomanip>
#include <iostream> // for cerr

//=============================================================================
//=============================================================================
class JsonParser
{
public:
    //-------------------------------------------------------------------------
    // in a key/value pair, the KVP can be:
    // Data (a string that can be interpreted as an int, float, etc.)
    // Array (of unnamed values)
    // Struct (array of named values)
    //-------------------------------------------------------------------------
    class KVP
    {
    public:
        // treat this as a map of name/value pairs
        KVP& operator [](const std::string& in_blockName);
        const KVP& operator [](const std::string& in_blockName) const;

        // treat this as an array of values
        KVP& operator [](const int in_index);
        const KVP& operator [](const int in_index) const;

        // interpret the data
        int32_t  asInt()    const;
        uint32_t asUInt()   const;
        float    asFloat()  const;
        bool     asBool()   const;
        double   asDouble() const;
        int64_t  asInt64()  const;
        uint64_t asUInt64() const;
        const std::string& asString() const;

        // enables iteration
        std::vector<KVP>::iterator begin() noexcept { return m_values.begin(); }
        std::vector<KVP>::iterator end() noexcept { return m_values.end(); }
        std::vector<KVP>::const_iterator begin() const noexcept { return m_values.begin(); }
        std::vector<KVP>::const_iterator end() const noexcept { return m_values.end(); }
        size_t size() const noexcept { return m_values.size(); }
        bool isMember(const std::string& in_blockName) const noexcept;

        // assignment via =
        template<typename T> KVP& operator = (const T in_v);

        // if not found, return a default value without adding it
        template<typename T> KVP get(const std::string in_name, T in_default) const noexcept;

        KVP() {}
        template<typename T> KVP(const T& in_t) { *this = in_t; }

        // the default assignment operator is by reference, but...
        // root["x"] = root["y"] has a race: root["y"] becomes invalid if root["x"] must be created
        //    because root is resized for "x" before reading the (by-reference) value of root["y"]
        // solution: pass param by value instead of reference to copy the source before assignment
        KVP& operator= (KVP o)
        {
            m_isString = o.m_isString;
            m_values = std::move(o.m_values);
            m_data = std::move(o.m_data);
            return *this;
        }

        void Write(std::ostream& out_s, uint32_t in_tab = 0) const;

    private:
        // KVP has an optional name + either a string value or an array of KVPs
        std::string m_name;
        std::string m_data;
        std::vector<KVP> m_values;
        bool m_isString{ false }; // only used when writing a file: remember if data was initally assigned as a string, will add quotes to output

        // used by JsonParser::Write()
        static constexpr uint32_t m_tabSize{ 2 };

        friend JsonParser;
    };

    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    JsonParser() {};
    JsonParser(const std::wstring& in_filePath) { m_readSuccess = Read(in_filePath); };
    bool GetReadSuccess() const { return m_readSuccess; }

    //-------------------------------------------------------------------------
    // read file
    //-------------------------------------------------------------------------
    bool Read(const std::wstring& in_filePath);

    //-------------------------------------------------------------------------
    // write to stream
    //-------------------------------------------------------------------------
    void Write(std::ostream& out_s) const { if (!out_s.bad()) { m_value.Write(out_s); } }

    KVP& GetRoot() { return m_value; }
    const KVP& GetRoot() const { return m_value; }

private:

    using Tokens = std::vector<std::string>;
    bool m_readSuccess{ true }; // only false if Read() failed
    const std::string m_symbols = "{}[],:";

    inline void ParseError(uint32_t in_pos)
    {
        std::string message = "Error in comment: unexpected character at position " + std::to_string(in_pos);
#ifdef _WINDOWS_
        ::MessageBoxA(0, message.c_str(), "Syntax Error", MB_OK);
#else
        std::cerr << message << std::endl;
#endif
        exit(-1);
    }

    inline void ParseError(const Tokens& tokens, uint32_t in_tokenIndex)
    {
        uint32_t firstToken = std::max((uint32_t)3, in_tokenIndex) - 3;
        uint32_t lastToken = std::min((uint32_t)tokens.size(), in_tokenIndex + 3);

        std::string message = "Error: unexpected token '" + tokens[in_tokenIndex - 1] + "', context:\n";
        for (uint32_t i = firstToken; i < lastToken; i++)
        {
            message += tokens[i] + " ";
        }
#ifdef _WINDOWS_
        ::MessageBoxA(0, message.c_str(), "Syntax Error", MB_OK);
#else
        std::cerr << message << std::endl;
#endif
        exit(-1);
    }

    //-------------------------------------------------------------------------
    // break string into an array of symbols or substrings
    //-------------------------------------------------------------------------
    inline void Tokenize(Tokens& out_tokens, std::string& in_stream)
    {
        const uint32_t numChars = (uint32_t)in_stream.size();

        for (uint32_t i = 0; i < numChars; i++)
        {
            char c = in_stream[i];

            if (std::isspace(c)) continue;

            // comments
            if ('/' == c)
            {
                i++;
                if (i >= numChars) break;
                switch (in_stream[i])
                {
                case '/':
                    while ((i < numChars) && (in_stream[i] != '\n')) { i++; } // c++ comment
                    break;
                case '*':
                    while (i < numChars) /* comment */
                    {
                        i++;
                        while ((i < numChars) && (in_stream[i] != '*')) { i++; }
                        i++;
                        if ((i < numChars) && ('/' == in_stream[i])) break;
                    }
                    break;
                default:
                    ParseError(i - 1);
                }
            }
            // symbols
            else if (m_symbols.find(c) != std::string::npos)
            {
                out_tokens.emplace_back(1, c);
            }
            // quoted strings (ignore spaces within)
            else if ('"' == c)
            {
                std::string s(1, c);
                while (++i < numChars)
                {
                    s.push_back(in_stream[i]);
                    if ('"' == in_stream[i]) break;
                }
                out_tokens.push_back(s);
            }
            // values
            else
            {
                std::string s;
                while ((i < numChars) && (!std::isspace(in_stream[i])) &&
                    (m_symbols.find(in_stream[i]) == std::string::npos))
                {
                    s.push_back(in_stream[i]);
                    i++;
                }
                out_tokens.push_back(s);
                if ((i < numChars) && (m_symbols.find(in_stream[i]) != std::string::npos))
                {
                    i--;  // Decrement i to process the symbol in the next iteration
                }
            }
        }
    }

    //-------------------------------------------------------------------------
    // values can be blocks, arrays, quoted strings, or strings
    //-------------------------------------------------------------------------
    uint32_t ReadValue(KVP& out_value, const Tokens& in_tokens, uint32_t in_tokenIndex)
    {
        auto& t = in_tokens[in_tokenIndex++];

        // there must be at least one more token
        if (in_tokenIndex >= in_tokens.size()) { ParseError(in_tokens, in_tokenIndex); }

        switch (t[0])
        {
        case '{': in_tokenIndex = ReadBlock(out_value, in_tokens, in_tokenIndex); break;
        case '[': in_tokenIndex = ReadArray(out_value, in_tokens, in_tokenIndex); break;
        case '"': out_value.m_data = t.substr(1, t.size() - 2);  out_value.m_isString = true; break;
        default:
            if (m_symbols.find(t[0]) != std::string::npos) { ParseError(in_tokens, in_tokenIndex); }
            out_value.m_data = t; // WARNING: does not validate the string is valid json
        }
        return in_tokenIndex;
    }

    //-------------------------------------------------------------------------
    // An "Array" is of the form NAME COLON [ comma-separated un-named VALUES within square brackets ]
    //     "nameOfArray" : [ value, value, { "block" : value }]
    //-------------------------------------------------------------------------
    uint32_t ReadArray(KVP& out_value, const Tokens& in_tokens, uint32_t in_tokenIndex)
    {
        if (']' == in_tokens[in_tokenIndex][0]) { return ++in_tokenIndex; } // 0-sized array

        while (1)
        {
            out_value.m_values.resize(out_value.m_values.size() + 1);
            in_tokenIndex = ReadValue(out_value.m_values.back(), in_tokens, in_tokenIndex);

            // must be at least one more token after the current token
            if (size_t(in_tokenIndex + 1) >= in_tokens.size()) { ParseError(in_tokens, in_tokenIndex); }

            // consume current token which must be end-bracket or comma
            auto& t = in_tokens[in_tokenIndex++];
            if (']' == t[0]) { break; }
            else if (',' != t[0]) { ParseError(in_tokens, in_tokenIndex); }
        }

        return in_tokenIndex;
    }

    //-------------------------------------------------------------------------
    // A "Block" is of the form NAME COLON { comma-separated named VALUES within curly brackets }
    // "nameOfBlock": {
    //    "name" : value,
    //    "array" : [ value, value, value],
    //    "struct" : { "name": value, /* etc. */ }
    //-------------------------------------------------------------------------
    uint32_t ReadBlock(KVP& out_value, const Tokens& in_tokens, uint32_t in_tokenIndex)
    {
        while (1)
        {
            if (size_t(in_tokenIndex + 3) >= in_tokens.size()) { ParseError(in_tokens, in_tokenIndex); }

            KVP v;
            {
                auto& t = in_tokens[in_tokenIndex++];
                if (t[0] != '"') ParseError(in_tokens, in_tokenIndex); // name must be quoted
                v.m_name = t.substr(1, t.size() - 2); // remove quotes from name
            }

            if (":" != in_tokens[in_tokenIndex++]) { ParseError(in_tokens, in_tokenIndex); }
            in_tokenIndex = ReadValue(v, in_tokens, in_tokenIndex);
            out_value.m_values.push_back(std::move(v));

            {
                auto& t = in_tokens[in_tokenIndex++];
                if ('}' == t[0]) { break; }
                else if (',' != t[0]) { ParseError(in_tokens, in_tokenIndex); }
            }
        }
        return in_tokenIndex;
    }

    KVP m_value;
};

//-------------------------------------------------------------------------
// read file
//-------------------------------------------------------------------------
inline bool JsonParser::Read(const std::wstring& in_filePath)
{
    std::ifstream ifs(in_filePath, std::ios::in | std::ifstream::binary);
    bool success = ifs.good();
    if (success)
    {
        std::string stream((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

        Tokens tokens;
        Tokenize(tokens, stream);

        if ("{" != tokens[0]) ParseError(tokens, 0);

        ReadBlock(m_value, tokens, 1);
    }

    return success;
}

//-------------------------------------------------------------------------
// in a name/value pair, the KVP can be:
// Data (a string that can be interpreted as an int, float, etc.)
// Array (of Values)
// Struct (map of name/value pairs)
//-------------------------------------------------------------------------
// treat this as a map of name/value pairs
inline JsonParser::KVP& JsonParser::KVP::operator [](const std::string& in_blockName)
{
    for (auto& v : m_values)
    {
        if (v.m_name == in_blockName)
        {
            return v;
        }
    }

    // didn't find it? create a new node. Useful for adding new values with =
    m_values.resize(m_values.size() + 1);
    m_values.back().m_name = in_blockName;
    m_data.clear(); // assign to values[] supercedes current data
    return m_values.back();
}

// constant version will not create new values
inline const JsonParser::KVP& JsonParser::KVP::operator [](const std::string& in_blockName) const
{
    for (auto& v : m_values)
    {
        if (v.m_name == in_blockName)
        {
            return v;
        }
    }
    return *this;
}

//-------------------------------------------------------------------------
// treat this as an array of values
//-------------------------------------------------------------------------
inline JsonParser::KVP& JsonParser::KVP::operator [](const int in_index)
{
    uint32_t index = (uint32_t)std::max(in_index, 0);
    if (index >= m_values.size()) // appending?
    {
        m_values.resize(in_index + 1);
        m_data.clear(); // assign to values[] supercedes current data
    }
    return m_values[in_index];
}

inline const JsonParser::KVP& JsonParser::KVP::operator [](const int in_index) const
{
    return m_values[in_index];
}

//-------------------------------------------------------------------------
// assignment creates or overwrites a value
//-------------------------------------------------------------------------
template<> inline JsonParser::KVP& JsonParser::KVP::operator=<std::string>(std::string in_v)
{
    m_isString = true;
    m_data = in_v;
    m_values.clear(); // assign to data supercedes current values[]
    return *this;
}

template<> inline JsonParser::KVP& JsonParser::KVP::operator=<const char*>(const char* in_v)
{
    *this = std::string(in_v); // use std::string assignment
    return *this;
}

template<> inline JsonParser::KVP& JsonParser::KVP::operator=(float in_v)
{
    m_isString = false;
    std::stringstream o;
    o << std::setprecision(std::numeric_limits<float>::digits10 + 1) << in_v;
    m_data = o.str();
    m_values.clear(); // assign to data supercedes current values[]
    return *this;
}

template<> inline JsonParser::KVP& JsonParser::KVP::operator=(double in_v)
{
    m_isString = false;
    std::stringstream o;
    o << std::setprecision(std::numeric_limits<double>::digits10 + 1) << in_v;
    m_data = o.str();
    m_values.clear(); // assign to data supercedes current values[]
    return *this;
}

template<typename T> inline JsonParser::KVP& JsonParser::KVP::operator=(T in_v)
{
    m_isString = false;
    m_data = std::to_string(in_v);
    m_values.clear(); // assign to data supercedes current values[]
    return *this;
}

//-------------------------------------------------------------------------
// non-destructive queries
//-------------------------------------------------------------------------
inline bool JsonParser::KVP::isMember(const std::string& in_blockName) const noexcept
{
    for (auto& v : m_values)
    {
        if (v.m_name == in_blockName)
        {
            return true;
        }
    }
    return false;
}

template<typename T> inline JsonParser::KVP JsonParser::KVP::get(const std::string in_name, T in_default) const noexcept
{
    auto& k = (*this)[in_name]; // const [] will not create kvp
    if (in_name == k.m_name)
    {
        return k;
    }
    return in_default;
}

//-------------------------------------------------------------------------
// conversions
//-------------------------------------------------------------------------
inline int32_t JsonParser::KVP::asInt() const
{
    if (m_data.length())
    {
        return std::stoi(m_data);
    }
    return 0;
}

inline uint32_t JsonParser::KVP::asUInt() const
{
    if (m_data.length())
    {
        return std::stoul(m_data);
    }
    return 0;
}

inline float JsonParser::KVP::asFloat() const
{
    if (m_data.length())
    {
        return std::stof(m_data);
    }
    return 0;
}

inline double JsonParser::KVP::asDouble() const
{
    if (m_data.length())
    {
        return std::stod(m_data);
    }
    return 0;
}

inline int64_t JsonParser::KVP::asInt64() const
{
    if (m_data.length())
    {
        return std::stoll(m_data);
    }
    return 0;
}

inline uint64_t JsonParser::KVP::asUInt64() const
{
    if (m_data.length())
    {
        return std::stoull(m_data);
    }
    return 0;
}

inline const std::string& JsonParser::KVP::asString() const
{
    return m_data;
}

inline bool JsonParser::KVP::asBool() const
{
    bool value = true;
    if (std::string::npos != m_data.find("false"))
    {
        value = false;
    }
    else // 0 and 0.0 are also false...
    {
        if (std::string::npos == m_data.find_first_not_of("0."))
        {
            // ok, it's only versions of 0 and 0.0
            if (0.0f == asFloat())
            {
                value = false;
            }
        }
    }

    return value;
}

//-------------------------------------------------------------------------
// write a KVP which may be a name:value, a block {} of named values, or an array [] of unnamed values
//-------------------------------------------------------------------------
inline void JsonParser::KVP::Write(std::ostream& out_s, uint32_t in_tab) const
{
    // start a new line for a named value or a unnamed block/array
    if (m_name.length() || (0 == m_data.length()))
    {
        out_s << std::endl << std::string(in_tab, ' ');
    }
    if (m_name.length())
    {
        out_s << "\"" << m_name << "\": ";
    }

    // if this has a value, print it and return
    if (m_data.length())
    {
        if (m_isString)
        {
            out_s << '\"' << m_data.c_str() << '\"';
        }
        else
        {
            out_s << m_data;
        }
    }

    // if there are multiple values, this is a block or an array
    else
    {
        char startChar = '{';
        char endChar = '}';
        // if first value is unnamed, assume array
        if ((0 == m_values.size()) || (0 == m_values[0].m_name.length()))
        {
            startChar = '[';
            endChar = ']';
        }

        out_s << startChar;
        for (uint32_t i = 0; i < m_values.size(); i++)
        {
            if (0 != i)
            {
                out_s << ", ";
            }
            m_values[i].Write(out_s, in_tab + m_tabSize);
        }
        // if the last value was an array or block, add a newline to prevent ]], ]}, etc.
        if (m_values.size() && m_values[m_values.size() - 1].size())
        {
            out_s << std::endl << std::string(in_tab, ' ');
        }
        out_s << endChar;
    }
}
