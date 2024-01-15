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

// Use format_str to format remaining arguments similar to std::format.
// All arguments should be convertible to std::string_view, and the only
// formatting templates supported are {} and {\d}.
template <typename... Args>
std::string myformat(std::string_view format_str, Args... args)
{
    std::initializer_list<std::string_view> arg_list{args...};
    auto arg = arg_list.begin();

    std::string result;
    const char* fmt = format_str.data();
    size_t fmt_size = format_str.size();
    size_t cur_arg = 0;    // the number of the next argument denoted by "{}"
    size_t start = 0;      // starting position of the current literal span in format_str

    for(size_t i=0; i+1 < fmt_size; i++)
    {
        // add to the result the literal string between the two arguments and then the next argument
        auto process_next_arg = [&] (size_t arg_num) {
            if(arg_num >= arg_list.size()) {
                throw std::runtime_error("Not enough arguments for myformat");
            }
            result += std::string_view(&fmt[start], i - start);
            result += arg[arg_num];
        };

        // "{}" in the format string is replaced by the next input argument
        if(fmt[i]=='{' && fmt[i+1]=='}')
        {
            process_next_arg(cur_arg++);
            start = i+2;
        }
        // "{\d}" in the format string is replaced by the input argument number #d
        else if(i+2<fmt_size && fmt[i]=='{' && isdigit(fmt[i+1]) && fmt[i+2]=='}')
        {
            process_next_arg(atoi(&fmt[i+1]));
            start = i+3;
        }
    }

    // add to the result the remainder of format_str
    result += std::string_view(&fmt[start], fmt_size - start);

    return result;
}
