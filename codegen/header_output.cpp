#include "header_output.hpp"

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "file_paths.hpp"
#include "logical_paths.hpp"

namespace easypb_codegen {
namespace {

easypb_schema::Diagnostic make_error(const std::string& file,
                                     const std::string& message)
{
    easypb_schema::Diagnostic diagnostic;
    diagnostic.file = file;
    diagnostic.location = easypb_schema::unknown_location();
    diagnostic.message = message;
    diagnostic.code = easypb_schema::DIAGNOSTIC_GENERIC;
    return diagnostic;
}

std::string ascii_lowercase(const std::string& text)
{
    std::string result = text;
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (result[i] >= 'A' && result[i] <= 'Z') {
            result[i] = static_cast<char>(result[i] + ('a' - 'A'));
        }
    }
    return result;
}

// The parent of an already-normalized absolute spelling. Never returns the
// path itself, so make_directories cannot be pointed at the output file.
std::string parent_of_absolute(const std::string& absolute)
{
    const std::size_t slash = absolute.rfind('/');
    if (slash == std::string::npos) return std::string();
    if (slash == 0) return "/";
    std::string parent = absolute.substr(0, slash);
#ifdef _WIN32
    if (parent.size() == 2 && parent[1] == ':' &&
        ((parent[0] >= 'a' && parent[0] <= 'z') ||
         (parent[0] >= 'A' && parent[0] <= 'Z'))) {
        parent += "/";
    }
#endif
    return parent;
}

} // namespace

bool publish_headers(const std::string& out_dir,
                     const std::vector<GeneratedFile>& files,
                     easypb_schema::Diagnostic& error)
{
    error = easypb_schema::Diagnostic();

    if (out_dir.empty()) {
        error = make_error(std::string(),
                           "cannot publish headers without an output directory");
        return false;
    }
    if (files.empty()) {
        error = make_error(out_dir, "no generated headers to publish");
        return false;
    }

    // 1. Names first: validation and duplicate detection are pure and leave
    //    the tree alone when they fail.
    std::set<std::string> names;
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::string reason;
        if (!easypb_schema::validate_logical_path(files[i].name, reason)) {
            error = make_error(files[i].name,
                               "generated header name \"" + files[i].name +
                                   "\" is not a safe output path: " + reason);
            return false;
        }
        if (!names.insert(files[i].name).second) {
            error = make_error(files[i].name,
                               "two targets map to one output file \"" +
                                   files[i].name + "\"");
            return false;
        }
    }

    // 2. Containment: lexical mapping below the output root, then the native
    //    walk. Anything that cannot be proven inside fails before a write.
    const std::string root_absolute = easypb_file::lexical_absolute(out_dir);
    if (root_absolute.empty()) {
        error = make_error(out_dir,
                           out_dir + ": cannot resolve the output directory");
        return false;
    }
    std::vector<std::string> targets;
    targets.reserve(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::string target = easypb_file::lexical_absolute(
            easypb_file::join_disk_path(out_dir, files[i].name));
        if (target.empty()) {
            error = make_error(files[i].name,
                               files[i].name +
                                   ": cannot resolve the output path");
            return false;
        }
        std::string reason;
        if (!easypb_file::confirm_fold_containment(target, root_absolute,
                                                   reason)) {
            error = make_error(
                target,
                reason.empty()
                    ? target + ": output path escapes the output directory \"" +
                          out_dir + "\""
                    : reason);
            return false;
        }
        targets.push_back(target);
    }

    // 3. Directories: the root exists before the case query can measure it.
    std::string directory_error;
    if (!easypb_file::make_directories(root_absolute, directory_error)) {
        error = make_error(out_dir, directory_error);
        return false;
    }

    // 4. Letter-case aliases are a collision only on a filesystem that folds
    //    case; the rule is measured instead of assumed, and a failed query is
    //    an error rather than a silent answer.
    bool case_sensitive = true;
    std::string case_error;
    if (!easypb_file::query_directory_case_sensitivity(root_absolute,
                                                       case_sensitive,
                                                       case_error)) {
        error = make_error(root_absolute, case_error);
        return false;
    }
    if (!case_sensitive) {
        std::map<std::string, std::string> folded;
        for (std::size_t i = 0; i < files.size(); ++i) {
            const std::string key = ascii_lowercase(files[i].name);
            const std::map<std::string, std::string>::const_iterator found =
                folded.find(key);
            if (found != folded.end() && found->second != files[i].name) {
                error = make_error(
                    files[i].name,
                    "output files \"" + found->second + "\" and \"" +
                        files[i].name +
                        "\" are one entry on this case-folding filesystem");
                return false;
            }
            folded[key] = files[i].name;
        }
    }

    // 5. Native identity: two planned outputs that are already one physical
    //    entry (a hardlink, or a case alias that survived) are a collision.
    std::map<std::pair<unsigned long, unsigned long long>, std::string>
        identities;
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::string status_error;
        const easypb_file::FileStatus status =
            easypb_file::disk_file_status(targets[i], status_error);
        if (status == easypb_file::FILE_STATUS_ERROR) {
            error = make_error(targets[i], status_error);
            return false;
        }
        if (status == easypb_file::FILE_STATUS_DIRECTORY) {
            error = make_error(targets[i],
                               targets[i] +
                                   ": output path is an existing directory");
            return false;
        }
        if (status != easypb_file::FILE_STATUS_FILE) continue;

        easypb_file::FileIdentity identity;
        std::string identity_error;
        if (!easypb_file::get_file_identity(targets[i], identity,
                                            identity_error)) {
            error = make_error(targets[i],
                               identity_error.empty()
                                   ? targets[i] +
                                         ": cannot read the output file identity"
                                   : identity_error);
            return false;
        }
        const std::pair<unsigned long, unsigned long long> key(
            identity.volume, identity.index);
        const std::map<std::pair<unsigned long, unsigned long long>,
                       std::string>::const_iterator found = identities.find(key);
        if (found != identities.end()) {
            error = make_error(
                targets[i],
                "output files \"" + found->second + "\" and \"" +
                    targets[i] + "\" are one physical entry");
            return false;
        }
        identities.insert(std::make_pair(key, targets[i]));
    }

    // 6. Write. Each file goes to a temporary sibling first; the target keeps
    //    its old bytes until the complete new text is on disk.
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::string parent = parent_of_absolute(targets[i]);
        if (!parent.empty()) {
            std::string parent_error;
            if (!easypb_file::make_directories(parent, parent_error)) {
                error = make_error(targets[i], parent_error);
                return false;
            }
        }
        if (!easypb_file::confirm_fold_containment(targets[i], root_absolute,
                                                   directory_error)) {
            error = make_error(targets[i],
                               directory_error.empty()
                                   ? targets[i] +
                                         ": output path escaped the output "
                                         "directory during publication"
                                   : directory_error);
            return false;
        }
        std::string write_error;
        if (!easypb_file::write_disk_file_atomic(targets[i], files[i].content,
                                                 write_error)) {
            error = make_error(targets[i], write_error);
            return false;
        }
    }
    return true;
}

} // namespace easypb_codegen
