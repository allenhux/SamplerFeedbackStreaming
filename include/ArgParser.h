//*********************************************************
//
// Copyright 2020 Intel Corporation 
//
// Permission is hereby granted, free of charge, to any 
// person obtaining a copy of this software and associated 
// documentation files(the "Software"), to deal in the Software 
// without restriction, including without limitation the rights 
// to use, copy, modify, merge, publish, distribute, sublicense, 
// and/or sell copies of the Software, and to permit persons to 
// whom the Software is furnished to do so, subject to the 
// following conditions :
// The above copyright notice and this permission notice shall 
// be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
// DEALINGS IN THE SOFTWARE.
//
//*********************************************************

/*-----------------------------------------------------------------------------
ArgParser

Parse arguments to a Windows application
On finding a match, calls custom code
Case is ignored while parsing

example:
This creates the parser, then searches for a few values.
The value is expected to follow the token.

runprogram.exe gRaVity 20.27 upIsDown dothing

float m_float = 0;
bool m_flipGravity = false;

ArgParser argParser;
argParser.AddArg(L"gravity", m_float);
argParser.AddArg(L"upisdown", m_flipGravity); // inverts m_flipGravity
argParser.AddArg(L"downisup", m_flipGravity, L"whoops!"); // inverts current value, includes help message
argParser.AddArg(L"dothing", [&]() { DoTheThing(); } ); // call custom function to handle param
argParser.AddArg(L"option", [&]() { DoOption(GetNextArg()); }, L"a function" ); // custom function with help message that reads the next arg from the command line
argParser.Parse();

after running, m_float=20.27 and m_flipGravity=true

-----------------------------------------------------------------------------*/
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers.
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <shellapi.h>
#include <sstream>
#include <iostream>

//-----------------------------------------------------------------------------
// parse command line
//-----------------------------------------------------------------------------
class ArgParser
{
public:
    // custom function to perform for a command line arg
    // use GetNextArg() to read the subsequent command line argument(s) as needed
    typedef std::function<void()> ArgFunction;
    const std::wstring& GetNextArg();

    // arg calls a custom function, optional help text
    void AddArg(std::wstring token, ArgFunction f, std::wstring description = L"");

    // prototype using supported types (see below), optional help text
    template<typename T> void AddArg(std::wstring token, T& out_value, std::wstring description = L"") = delete;

    // prototype calls a custom function, optional help text, help text includes default value
    template<typename T> void AddArg(std::wstring token, ArgFunction f, T default_value, std::wstring description = L"");

    void Parse();

    ArgParser();
private:
    class ArgPair
    {
    public:
        ArgPair(std::wstring s, ArgFunction f) : m_arg(ToLower(s)), m_func(f) {}
        bool TestEqual(const std::wstring& in_arg)
        {
            bool found = (m_arg == in_arg);
            if (found) { m_func(); }
            return found;
        }
    private:
        const std::wstring m_arg;
        const ArgFunction m_func;
    };

    std::vector<ArgPair> m_args;
    std::wstringstream m_help;

    std::vector<std::wstring> m_cmdLineArgs;
    int m_argIndex{ 0 };

    // MSDN: In order for _tolower to give the expected results, __isascii and isupper must both return nonzero.
    static std::wstring ToLower(std::wstring s)
    {
        for (auto& c : s)
        {
            if (::iswascii(c) && ::iswupper(c))
            {
                c = ::towlower(c);
            }
        }
        return s;
    }
};

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline ArgParser::ArgParser()
{
    int numArgs = 0;
    const LPWSTR* cmdLine = CommandLineToArgvW(GetCommandLineW(), &numArgs);
    m_cmdLineArgs = std::vector<std::wstring>(&cmdLine[1], &cmdLine[numArgs]);
    for (auto& arg : m_cmdLineArgs)
    {
        arg = ToLower(arg);
    }
}

//-----------------------------------------------------------------------------
// from GetCommandLine()
//-----------------------------------------------------------------------------
inline const std::wstring& ArgParser::GetNextArg()
{
    if (m_argIndex == m_cmdLineArgs.size())
    {
        std::wcerr << "Not enough command line arguments\n";
        exit(0);
    }
    const std::wstring& s = m_cmdLineArgs[m_argIndex];
    m_argIndex++;
    return s;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline void ArgParser::AddArg(std::wstring s, ArgParser::ArgFunction f, std::wstring description)
{
    m_args.push_back(ArgPair(s, f));
    m_help << s << ": " << description << std::endl;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
inline void ArgParser::Parse()
{
    if ((1 == m_cmdLineArgs.size()) && (std::wstring(L"?") == m_cmdLineArgs[0]))
    {
        BOOL allocConsole = AllocConsole(); // returns false for console applications
        if (allocConsole)
        {
            FILE* pOutStream;
            ::freopen_s(&pOutStream, "CONOUT$", "w", stdout);
            std::wcout.clear();
        }

        std::wcout << m_help.str();

        if (allocConsole)
        {
            ::system("pause");
        }

        exit(0);
    }

    while (m_argIndex < m_cmdLineArgs.size())
    {
        const std::wstring& s = GetNextArg();
        for (auto& arg : m_args)
        {
            if (arg.TestEqual(s)) { break; }
        }
    }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
template<typename T> inline void ArgParser::AddArg(std::wstring s, ArgFunction f, T default_value, std::wstring d)
{
    std::wstringstream w;
    w << d << " (default: " << default_value << ") ";
    AddArg(s, f, w.str());
}

template<> inline void ArgParser::AddArg(std::wstring s, ArgFunction f, bool default_value, std::wstring d)
{
    std::wstringstream w;
    std::string b = default_value ? "True" : "False";
    w << d << " (default: " << b.c_str() << ") ";
    AddArg(s, f, w.str());
}

template<> inline void ArgParser::AddArg(std::wstring arg, long& value, std::wstring desc) { AddArg(arg, [&]() { value = std::stol(GetNextArg()); }, value, desc); }
template<> inline void ArgParser::AddArg(std::wstring arg, UINT& value, std::wstring desc) { AddArg(arg, [&]() { value = std::stoul(GetNextArg()); }, value, desc); }
template<> inline void ArgParser::AddArg(std::wstring arg, int& value, std::wstring desc) { AddArg(arg, [&]() { value = std::stoi(GetNextArg()); }, value, desc); }
template<> inline void ArgParser::AddArg(std::wstring arg, float& value, std::wstring desc) { AddArg(arg, [&]() { value = std::stof(GetNextArg()); }, value, desc); }
template<> inline void ArgParser::AddArg(std::wstring arg, bool& value, std::wstring desc) { AddArg(arg, [&]() { value = !value; }, value, desc); }
template<> inline void ArgParser::AddArg(std::wstring arg, std::wstring& value, std::wstring desc) { AddArg(arg, [&]() { value = GetNextArg(); }, value, desc); }
template<> inline void ArgParser::AddArg(std::wstring arg, double& value, std::wstring desc) { AddArg(arg, [&]() { value = std::stod(GetNextArg()); }, value, desc); }
template<> inline void ArgParser::AddArg(std::wstring arg, INT64& value, std::wstring desc) { AddArg(arg, [&]() { value = std::stoll(GetNextArg()); }, value, desc); }
template<> inline void ArgParser::AddArg(std::wstring arg, UINT64& value, std::wstring desc) { AddArg(arg, [&]() { value = std::stoull(GetNextArg()); }, value, desc); }
