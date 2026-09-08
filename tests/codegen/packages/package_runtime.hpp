#ifndef EASYPB_CODEGEN_PACKAGE_RUNTIME_HPP_INCLUDED
#define EASYPB_CODEGEN_PACKAGE_RUNTIME_HPP_INCLUDED

#include <string>

// Package namespaces do not change the established insertion-macro names.
#define EASYPB_Outer_EXTRA_FIELDS \
    ::int32_t injected = 0; \
    bool post_decoded = false;
#define EASYPB_Outer_EXTRA_ENCODING(pb, message) \
    (pb).put_int32(100, (message).injected);
#define EASYPB_Outer_EXTRA_DECODING(pb, message) \
    case 100: (pb).get_int32(&(message).injected); break;
#define EASYPB_Outer_EXTRA_POST_DECODING(pb, message) \
    (message).post_decoded = true;

#include "names.generated.hpp"
#include "alpha.generated.hpp"
#include "beta.generated.hpp"
#include "shadow.generated.hpp"
#include "module-package.generated.hpp"
#include "import-package.generated.hpp"

std::string encode_outer_in_helper(const foo::bar::Outer& value);
foo::bar::Outer decode_outer_in_helper(const std::string& wire);

#endif
