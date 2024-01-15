// Returns a version of 'str' where every occurrence of
// 'find' is substituted by 'replace'.
// - http://stackoverflow.com/questions/20406744/
std::string string_replace_all(
    std::string_view str ,   // where to work
    std::string_view find ,  // substitute 'find'
    std::string_view replace //      by 'replace'
) {
    using namespace std;
    string result;
    size_t find_len = find.size();
    size_t pos, from = 0;
    while( string::npos != ( pos=str.find(find,from) ) ) {
        result.append( str, from, pos-from );
        result.append( replace );
        from = pos + find_len;
    }
    result.append( str, from , string::npos );
    return result;
}


// An object that formats input arguments similar to std::format
struct Formatter
{
    // The formatter holds N+1 literal strings
    // and N numbers of arguments inserted between the literals
    std::vector<std::string> literal;
    std::vector<int> argument;

    // Apply formatting string to the provided arguments (which should be convertible to string_view)
    template <typename... Args>
    std::string format(Args... args)
    {
        std::initializer_list<std::string_view> list{std::forward<Args>(args)...};
        auto arg = list.begin();

        std::string result;

        for(int i=0; i < argument.size(); i++)
        {
            if(argument[i] >= list.size()) {
                throw std::runtime_error("Not enough arguments for Formatter");
            }
            result += literal[i];
            result += arg[argument[i]];
        }
        result += literal[argument.size()];

        return result;
    }
};

Formatter make_format(std::string_view format_str)
{
    Formatter result;
    const char* s = format_str.data();
    size_t size = format_str.size();
    size_t arg_num = 0;    // the number of the next argument denoted by "{}"
    size_t start = 0;      // starting position of the current literal span

    for(size_t i=0; i+1 < size; i++)
    {
        // "{}" in the format string is replaced by the next input argument
        if(s[i]=='{' && s[i+1]=='}')
        {
            // save the literal string between two arguments and the number of the next argument
            result.literal.push_back({s + start, i - start});
            result.argument.push_back(arg_num++);
            start = i+2;
        }
        // "{\d}" in the format string is replaced by the input argument number #d
        else if(i+2<size && s[i]=='{' && isdigit(s[i+1]) && s[i+2]=='}')
        {
            arg_num = atoi(s + i + 1);
            result.literal.push_back({s + start, i - start});
            result.argument.push_back(arg_num++);
            start = i+3;
        }
    }

    // save the remainder of format_str
    result.literal.push_back({s + start, size - start});

    return result;
}
