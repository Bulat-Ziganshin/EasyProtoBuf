const char* USAGE =
"Generator of C++ code from compiled ProtoBuf schema\n"
"  Usage: codegen [options] file.pbs...\n";

#include "popl.hpp"

#include "codegen.cpp"


std::vector<std::string> parse_cmdline(int argc, char** argv)
{
    using namespace popl;

    OptionParser op("Allowed options");

    auto help_option  = op.add<Switch>("h", "help", "produce help message");
    auto groff_option = op.add<Switch>("", "groff", "produce groff formatted help message");
    auto bash_option  = op.add<Switch>("", "bash", "produce bash completion script");

    op.add<Switch>("c", "no-class", "don't generate C++ struct", &option.no_class);
    op.add<Switch>("d", "no-decoder", "don't generate decoder", &option.no_decoder);
    op.add<Switch>("e", "no-encoder", "don't generate encoder", &option.no_encoder);

    op.add<Switch>("f", "no-has-fields", "don't generate has_* fields", &option.no_has_fields);
    op.add<Switch>("", "no-required", "ignore 'required' attribute", &option.no_required);
    op.add<Switch>("", "no-default-values", "ignore default values", &option.no_default_values);

    op.add<Switch>("p", "packed", "make all repeated fields packed when allowed", &option.packed);
    op.add<Switch>("", "no-packed", "make all repeated fields non-packed", &option.no_packed);

    op.add<Value<std::string>>("s", "string-type", "C++ type for string/bytes fields", "std::string", &option.cpp_string_type);
    op.add<Value<std::string>>("r", "repeated-type", "C++ container type for repeated fields", "std::vector", &option.cpp_repeated_type);


    op.parse(argc, argv);

    // print auto-generated help message
    if (groff_option->is_set()) {
        GroffOptionPrinter option_printer(&op);
        std::cout << option_printer.print();
        return {};
    }
    if (bash_option->is_set()) {
        BashCompletionOptionPrinter option_printer(&op, "codegen");
        std::cout << option_printer.print();
        return {};
    }
    if (help_option->count() || ! op.non_option_args().size()) {
        std::cout << USAGE << "\n";
        std::cout << op << "\n";
    }

    if(option.no_has_fields) {
        option.no_required = true;  // we can't check presence of a required field without employing the corresponding has_* field
    }
    return op.non_option_args();
}


int main(int argc, char** argv)
{
    try {
    	auto filenames = parse_cmdline(argc, argv);

        for (auto &&filename : filenames)
        {
            std::ifstream ifs(filename, std::ios::binary);
            std::string str(std::istreambuf_iterator<char>{ifs}, {});

            FileDescriptorSet proto;
            proto.decode(str);

            std::cout << std::format(FILE_TEMPLATE, filename);
            generator(proto);
        }
    }
    catch (const std::exception& e) {
        fprintf(stderr, "Internal error: %s\n", e.what());
    }

    return 0;
}
