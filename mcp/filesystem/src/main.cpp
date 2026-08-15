#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace
{
namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::uintmax_t maximumFileBytes = 1'048'576;
constexpr std::size_t maximumDirectoryEntries = 1'000;
constexpr std::size_t maximumSearchResults = 500;
constexpr std::size_t maximumTreeEntries = 2'000;

struct Root
{
    fs::path path;
    bool writable = false;
};

struct ToolResponse
{
    std::string text;
    bool isError = false;
};

#ifdef _WIN32
std::string wideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const auto size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

fs::path pathFromUtf8(const std::string& value)
{
    if (value.empty()) return {};
    const auto size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return fs::path(result);
}
#else
fs::path pathFromUtf8(const std::string& value)
{
    return fs::path(value);
}
#endif

std::string pathToUtf8(const fs::path& path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

bool pathComponentEquals(const fs::path& left, const fs::path& right)
{
#ifdef _WIN32
    const auto& leftValue = left.native();
    const auto& rightValue = right.native();
    return CompareStringOrdinal(
               leftValue.data(), static_cast<int>(leftValue.size()),
               rightValue.data(), static_cast<int>(rightValue.size()),
               TRUE) == CSTR_EQUAL;
#else
    return left == right;
#endif
}

bool isWithin(const fs::path& candidate, const fs::path& root)
{
    const auto normalizedCandidate = candidate.lexically_normal();
    const auto normalizedRoot = root.lexically_normal();
    auto candidatePart = normalizedCandidate.begin();
    for (auto rootPart = normalizedRoot.begin();
         rootPart != normalizedRoot.end(); ++rootPart, ++candidatePart)
    {
        if (candidatePart == normalizedCandidate.end() ||
            !pathComponentEquals(*candidatePart, *rootPart))
            return false;
    }
    return true;
}

fs::path resolvePath(const std::string& requestedPath, bool writeAccess,
                     const std::vector<Root>& roots, std::string& errorMessage)
{
    auto requested = pathFromUtf8(requestedPath);
    if (requested.empty())
    {
        errorMessage = "Path must be non-empty UTF-8.";
        return {};
    }
    if (!requested.is_absolute())
    {
        if (requested.has_root_name() || requested.has_root_directory())
        {
            errorMessage = "Path has an incomplete root prefix.";
            return {};
        }
        const auto root = std::find_if(
            roots.cbegin(), roots.cend(), [writeAccess](const Root& candidate)
            { return !writeAccess || candidate.writable; });
        if (root == roots.cend())
        {
            errorMessage = writeAccess
                               ? "No writable filesystem root is available."
                               : "No filesystem root is available.";
            return {};
        }
        requested = root->path / requested;
    }

    std::error_code error;
    fs::path resolved;
    const auto status = fs::symlink_status(requested, error);
    if (!error && status.type() != fs::file_type::not_found)
    {
        resolved = fs::canonical(requested, error);
    }
    else
    {
        error.clear();
        std::vector<fs::path> missingParts;
        auto cursor = requested;
        while (!cursor.empty() && !fs::exists(cursor, error))
        {
            error.clear();
            if (cursor == cursor.root_path()) break;
            missingParts.push_back(cursor.filename());
            const auto parent = cursor.parent_path();
            if (parent == cursor) break;
            cursor = parent;
        }
        resolved = fs::canonical(cursor, error);
        if (!error)
        {
            for (auto part = missingParts.rbegin(); part != missingParts.rend();
                 ++part)
                resolved /= *part;
            resolved = resolved.lexically_normal();
        }
    }
    if (error || resolved.empty())
    {
        errorMessage = "Unable to resolve path.";
        return {};
    }

    for (const auto& root : roots)
    {
        if ((!writeAccess || root.writable) && isWithin(resolved, root.path))
            return resolved;
    }
    errorMessage = writeAccess ? "Path is outside writable roots."
                               : "Path is outside allowed roots.";
    return {};
}

Json stringProperty(const std::string& description)
{
    return {{"type", "string"}, {"description", description}};
}

Json objectSchema(Json properties,
                  const std::vector<std::string>& required = {})
{
    Json schema{{"type", "object"},
                {"properties", std::move(properties)},
                {"additionalProperties", false}};
    if (!required.empty()) schema["required"] = required;
    return schema;
}

Json toolDefinition(const std::string& name, const std::string& description,
                    Json schema)
{
    return {{"name", name},
            {"description", description},
            {"inputSchema", std::move(schema)}};
}

Json toolDefinitions()
{
    const auto path = stringProperty(
        "Absolute path within an authorized root, or a path relative to the "
        "first authorized root");
    const auto content = stringProperty("UTF-8 file content");
    return Json::array(
        {toolDefinition("read_text_file", "Read a UTF-8 text file.",
                        objectSchema({{"path", path}}, {"path"})),
         toolDefinition(
             "read_multiple_files", "Read multiple UTF-8 text files.",
             objectSchema({{"paths", {{"type", "array"}, {"items", path}}}},
                          {"paths"})),
         toolDefinition("list_directory", "List files and directories.",
                        objectSchema({{"path", path}}, {"path"})),
         toolDefinition(
             "directory_tree", "Show a bounded recursive directory tree.",
             objectSchema(
                 {{"path", path},
                  {"maxDepth",
                   {{"type", "integer"}, {"minimum", 1}, {"maximum", 8}}}},
                 {"path"})),
         toolDefinition(
             "search_files", "Search file names below a directory.",
             objectSchema(
                 {{"path", path},
                  {"pattern",
                   stringProperty("Case-insensitive text contained in the "
                                  "file name")}},
                 {"path", "pattern"})),
         toolDefinition("get_file_info", "Get file or directory metadata.",
                        objectSchema({{"path", path}}, {"path"})),
         toolDefinition("list_allowed_directories",
                        "List authorized filesystem roots.", objectSchema({})),
         toolDefinition("create_directory", "Create a directory tree.",
                        objectSchema({{"path", path}}, {"path"})),
         toolDefinition("write_file", "Atomically create or replace a file.",
                        objectSchema({{"path", path}, {"content", content}},
                                     {"path", "content"})),
         toolDefinition(
             "edit_file", "Replace one exact text occurrence in a file.",
             objectSchema(
                 {{"path", path},
                  {"oldText", stringProperty("Text to replace exactly once")},
                  {"newText", stringProperty("Replacement text")}},
                 {"path", "oldText", "newText"})),
         toolDefinition("move_file",
                        "Move a file or directory without overwriting.",
                        objectSchema({{"source", path}, {"destination", path}},
                                     {"source", "destination"})),
         toolDefinition("delete_file", "Permanently delete one file.",
                        objectSchema({{"path", path}}, {"path"})),
         toolDefinition("delete_directory", "Delete one empty directory.",
                        objectSchema({{"path", path}}, {"path"}))});
}

std::string argumentString(const Json& arguments, const char* name)
{
    const auto value = arguments.find(name);
    return value != arguments.end() && value->is_string()
               ? value->get<std::string>()
               : std::string{};
}

ToolResponse readFile(const std::string& requestedPath,
                      const std::vector<Root>& roots)
{
    std::string errorMessage;
    const auto path = resolvePath(requestedPath, false, roots, errorMessage);
    if (path.empty()) return {errorMessage, true};
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error)
        return {"Path is not a file.", true};
    const auto size = fs::file_size(path, error);
    if (error) return {"Unable to determine file size.", true};
    if (size > maximumFileBytes)
        return {"File exceeds the 1 MiB read limit.", true};

    std::ifstream file(path, std::ios::binary);
    if (!file) return {"Unable to open file for reading.", true};
    std::string contents(static_cast<std::size_t>(size), '\0');
    file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!file && !file.eof()) return {"Unable to read file.", true};
    contents.resize(static_cast<std::size_t>(file.gcount()));
    return {std::move(contents), false};
}

std::vector<fs::directory_entry> sortedDirectoryEntries(const fs::path& path,
                                                        std::error_code& error)
{
    std::vector<fs::directory_entry> entries;
    fs::directory_iterator iterator(path, fs::directory_options::none, error);
    const fs::directory_iterator end;
    while (!error && iterator != end)
    {
        entries.push_back(*iterator);
        iterator.increment(error);
    }
    if (error) return {};
    std::sort(entries.begin(), entries.end(),
              [](const auto& left, const auto& right)
              {
                  std::error_code leftError;
                  std::error_code rightError;
                  const auto leftDirectory = left.is_directory(leftError);
                  const auto rightDirectory = right.is_directory(rightError);
                  if (leftDirectory != rightDirectory)
                      return leftDirectory > rightDirectory;
                  return pathToUtf8(left.path().filename()) <
                         pathToUtf8(right.path().filename());
              });
    return entries;
}

ToolResponse listDirectory(const std::string& requestedPath,
                           const std::vector<Root>& roots)
{
    std::string errorMessage;
    const auto path = resolvePath(requestedPath, false, roots, errorMessage);
    if (path.empty()) return {errorMessage, true};
    std::error_code error;
    if (!fs::is_directory(path, error) || error)
        return {"Path is not a directory.", true};
    const auto entries = sortedDirectoryEntries(path, error);
    if (error) return {"Unable to list directory.", true};

    std::ostringstream output;
    const auto count = std::min(entries.size(), maximumDirectoryEntries);
    for (std::size_t index = 0; index < count; ++index)
    {
        std::error_code typeError;
        output << (entries[index].is_directory(typeError) ? "[DIR] "
                                                          : "[FILE] ")
               << pathToUtf8(entries[index].path().filename()) << '\n';
    }
    if (entries.size() > count)
        output << "... " << entries.size() - count << " more entries omitted\n";
    auto text = output.str();
    if (!text.empty()) text.pop_back();
    return {std::move(text), false};
}

void appendTree(const fs::path& path, const std::string& prefix, int depth,
                int maxDepth, std::vector<std::string>& lines,
                std::size_t& entryCount)
{
    if (depth > maxDepth || entryCount >= maximumTreeEntries) return;
    std::error_code error;
    const auto entries = sortedDirectoryEntries(path, error);
    if (error) return;
    for (const auto& entry : entries)
    {
        if (entryCount >= maximumTreeEntries) return;
        std::error_code typeError;
        const auto directory = entry.is_directory(typeError);
        lines.push_back(prefix + (directory ? "[DIR] " : "[FILE] ") +
                        pathToUtf8(entry.path().filename()));
        ++entryCount;
        if (directory && !entry.is_symlink(typeError))
            appendTree(entry.path(), prefix + "  ", depth + 1, maxDepth, lines,
                       entryCount);
    }
}

std::string joinLines(const std::vector<std::string>& lines)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        if (index > 0) output << '\n';
        output << lines[index];
    }
    return output.str();
}

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character)
                   { return static_cast<char>(std::tolower(character)); });
    return value;
}

std::string lastModifiedIso(const fs::file_time_type& value)
{
    const auto systemTime =
        std::chrono::time_point_cast<std::chrono::milliseconds>(
            value - fs::file_time_type::clock::now() +
            std::chrono::system_clock::now());
    const auto seconds =
        std::chrono::time_point_cast<std::chrono::seconds>(systemTime);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(systemTime -
                                                              seconds)
            .count();
    const auto time = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3)
           << std::setfill('0') << milliseconds << 'Z';
    return output.str();
}

bool writeAtomically(const fs::path& path, const std::string& contents,
                     std::string& errorMessage)
{
    static std::atomic_uint64_t sequence{0};
    auto temporary = path;
    temporary += pathFromUtf8(".qtllm-tmp-" + std::to_string(++sequence));
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            errorMessage = "Unable to open temporary file for writing.";
            return false;
        }
        file.write(contents.data(),
                   static_cast<std::streamsize>(contents.size()));
        file.flush();
        if (!file)
        {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            errorMessage = "Unable to write temporary file.";
            return false;
        }
    }

#ifdef _WIN32
    const auto replaced = MoveFileExW(temporary.c_str(), path.c_str(),
                                      MOVEFILE_REPLACE_EXISTING |
                                          MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!replaced)
    {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        errorMessage = "Unable to replace destination file.";
        return false;
    }
#else
    std::error_code error;
    fs::rename(temporary, path, error);
    if (error)
    {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        errorMessage = "Unable to replace destination file.";
        return false;
    }
#endif
    return true;
}

ToolResponse callTool(const std::string& name, const Json& arguments,
                      const std::vector<Root>& roots)
{
    try
    {
        if (!arguments.is_object())
            return {"Tool arguments must be an object.", true};
        if (name == "read_text_file")
            return readFile(argumentString(arguments, "path"), roots);
        if (name == "read_multiple_files")
        {
            const auto paths = arguments.find("paths");
            if (paths == arguments.end() || !paths->is_array())
                return {"paths must be an array.", true};
            Json result = Json::object();
            for (const auto& value : *paths)
            {
                if (!value.is_string()) continue;
                const auto path = value.get<std::string>();
                const auto response = readFile(path, roots);
                result[path] = response.isError ? "Error: " + response.text
                                                : response.text;
            }
            return {result.dump(2, ' ', false, Json::error_handler_t::replace),
                    false};
        }
        if (name == "list_directory")
            return listDirectory(argumentString(arguments, "path"), roots);
        if (name == "directory_tree")
        {
            std::string errorMessage;
            const auto path = resolvePath(argumentString(arguments, "path"),
                                          false, roots, errorMessage);
            if (path.empty()) return {errorMessage, true};
            std::error_code error;
            if (!fs::is_directory(path, error) || error)
                return {"Path is not a directory.", true};
            auto maxDepth = 4;
            const auto depth = arguments.find("maxDepth");
            if (depth != arguments.end() && depth->is_number_integer())
                maxDepth = std::clamp(depth->get<int>(), 1, 8);
            std::vector<std::string> lines;
            std::size_t entryCount = 0;
            appendTree(path, {}, 1, maxDepth, lines, entryCount);
            if (entryCount >= maximumTreeEntries)
                lines.emplace_back("... tree output truncated");
            return {joinLines(lines), false};
        }
        if (name == "search_files")
        {
            std::string errorMessage;
            const auto path = resolvePath(argumentString(arguments, "path"),
                                          false, roots, errorMessage);
            if (path.empty()) return {errorMessage, true};
            const auto pattern =
                lowerAscii(argumentString(arguments, "pattern"));
            if (pattern.empty())
                return {"Search pattern must not be empty.", true};
            std::error_code error;
            if (!fs::is_directory(path, error) || error)
                return {"Path is not a directory.", true};
            std::vector<std::string> matches;
            fs::recursive_directory_iterator iterator(
                path, fs::directory_options::skip_permission_denied, error);
            const fs::recursive_directory_iterator end;
            while (!error && iterator != end &&
                   matches.size() < maximumSearchResults)
            {
                if (lowerAscii(pathToUtf8(iterator->path().filename()))
                        .find(pattern) != std::string::npos)
                    matches.push_back(
                        pathToUtf8(iterator->path().lexically_relative(path)));
                iterator.increment(error);
            }
            return {joinLines(matches), false};
        }
        if (name == "get_file_info")
        {
            std::string errorMessage;
            const auto path = resolvePath(argumentString(arguments, "path"),
                                          false, roots, errorMessage);
            if (path.empty()) return {errorMessage, true};
            std::error_code error;
            const auto status = fs::status(path, error);
            if (error) return {"Unable to read file metadata.", true};
            const auto directory = fs::is_directory(status);
            const auto size =
                directory ? std::uintmax_t{0} : fs::file_size(path, error);
            if (error) return {"Unable to read file size.", true};
            const auto modified = fs::last_write_time(path, error);
            if (error) return {"Unable to read modification time.", true};
            const auto permissions = status.permissions();
            const auto readable =
                (permissions & (fs::perms::owner_read | fs::perms::group_read |
                                fs::perms::others_read)) != fs::perms::none;
            const auto writable =
                (permissions &
                 (fs::perms::owner_write | fs::perms::group_write |
                  fs::perms::others_write)) != fs::perms::none;
            const Json details{{"path", pathToUtf8(path)},
                               {"type", directory ? "directory" : "file"},
                               {"sizeBytes", size},
                               {"lastModified", lastModifiedIso(modified)},
                               {"readable", readable},
                               {"writable", writable}};
            return {details.dump(2), false};
        }
        if (name == "list_allowed_directories")
        {
            Json result = Json::array();
            for (const auto& root : roots)
                result.push_back(
                    {{"path", pathToUtf8(root.path)},
                     {"access", root.writable ? "readWrite" : "readOnly"}});
            return {result.dump(2), false};
        }

        const auto requestedPath = argumentString(arguments, "path");
        if (name == "create_directory")
        {
            std::string errorMessage;
            const auto path =
                resolvePath(requestedPath, true, roots, errorMessage);
            if (path.empty()) return {errorMessage, true};
            std::error_code error;
            fs::create_directories(path, error);
            if (error || !fs::is_directory(path))
                return {"Unable to create directory.", true};
            return {"Created directory: " + pathToUtf8(path), false};
        }
        if (name == "write_file")
        {
            const auto contents = argumentString(arguments, "content");
            if (contents.size() > maximumFileBytes)
                return {"Content exceeds the 1 MiB write limit.", true};
            std::string errorMessage;
            const auto path =
                resolvePath(requestedPath, true, roots, errorMessage);
            if (path.empty()) return {errorMessage, true};
            if (!writeAtomically(path, contents, errorMessage))
                return {errorMessage, true};
            return {"Wrote file: " + pathToUtf8(path), false};
        }
        if (name == "edit_file")
        {
            std::string errorMessage;
            const auto path =
                resolvePath(requestedPath, true, roots, errorMessage);
            if (path.empty()) return {errorMessage, true};
            const auto existing = readFile(requestedPath, roots);
            if (existing.isError) return existing;
            const auto oldText = argumentString(arguments, "oldText");
            const auto newText = argumentString(arguments, "newText");
            if (oldText.empty()) return {"oldText must not be empty.", true};
            const auto first = existing.text.find(oldText);
            if (first == std::string::npos ||
                existing.text.find(oldText, first + oldText.size()) !=
                    std::string::npos)
                return {"oldText must occur exactly once in the file.", true};
            auto updated = existing.text;
            updated.replace(first, oldText.size(), newText);
            if (updated.size() > maximumFileBytes)
                return {"Edited file exceeds the 1 MiB limit.", true};
            if (!writeAtomically(path, updated, errorMessage))
                return {errorMessage, true};
            return {"Edited file: " + pathToUtf8(path), false};
        }
        if (name == "move_file")
        {
            std::string errorMessage;
            const auto source = resolvePath(argumentString(arguments, "source"),
                                            true, roots, errorMessage);
            if (source.empty()) return {errorMessage, true};
            const auto destination =
                resolvePath(argumentString(arguments, "destination"), true,
                            roots, errorMessage);
            if (destination.empty()) return {errorMessage, true};
            std::error_code error;
            if (fs::exists(destination, error))
                return {"Destination already exists.", true};
            fs::rename(source, destination, error);
            if (error) return {"Unable to move path.", true};
            return {"Moved " + pathToUtf8(source) + " to " +
                        pathToUtf8(destination),
                    false};
        }
        if (name == "delete_file")
        {
            std::string errorMessage;
            const auto path =
                resolvePath(requestedPath, true, roots, errorMessage);
            if (path.empty()) return {errorMessage, true};
            std::error_code error;
            if (!fs::is_regular_file(path, error) || !fs::remove(path, error) ||
                error)
                return {"Unable to delete file.", true};
            return {"Deleted file: " + pathToUtf8(path), false};
        }
        if (name == "delete_directory")
        {
            std::string errorMessage;
            const auto path =
                resolvePath(requestedPath, true, roots, errorMessage);
            if (path.empty()) return {errorMessage, true};
            std::error_code error;
            if (!fs::is_directory(path, error) || !fs::remove(path, error) ||
                error)
                return {"Unable to delete directory; it must be empty.", true};
            return {"Deleted empty directory: " + pathToUtf8(path), false};
        }
        return {"Unknown filesystem tool: " + name, true};
    }
    catch (const std::exception& exception)
    {
        return {std::string("Invalid tool arguments: ") + exception.what(),
                true};
    }
}

Json toolResult(const ToolResponse& response)
{
    return {
        {"content", Json::array({{{"type", "text"}, {"text", response.text}}})},
        {"isError", response.isError}};
}

void writeMessage(const Json& message)
{
    std::cout << message.dump(-1, ' ', false, Json::error_handler_t::replace)
              << '\n'
              << std::flush;
}

void writeError(const Json& id, int code, const std::string& message)
{
    writeMessage({{"jsonrpc", "2.0"},
                  {"id", id},
                  {"error", {{"code", code}, {"message", message}}}});
}

bool addRoot(const fs::path& path, bool writable, std::vector<Root>& roots,
             std::string& errorMessage)
{
    std::error_code error;
    if (!path.is_absolute() || !fs::is_directory(path, error) || error)
    {
        errorMessage =
            "Filesystem root is not a directory: " + pathToUtf8(path);
        return false;
    }
    const auto canonical = fs::canonical(path, error);
    if (error || canonical.empty())
    {
        errorMessage = "Unable to resolve filesystem root: " + pathToUtf8(path);
        return false;
    }
    for (auto& root : roots)
    {
        if (isWithin(root.path, canonical) && isWithin(canonical, root.path))
        {
            root.writable = root.writable || writable;
            return true;
        }
    }
    roots.push_back({canonical, writable});
    return true;
}

int run(const std::vector<std::string>& arguments)
{
    std::vector<Root> roots;
    for (std::size_t index = 1; index < arguments.size(); ++index)
    {
        if (arguments[index] == "--version")
        {
            std::cout << "0.2.0\n";
            return 0;
        }
        if (arguments[index] == "--help")
        {
            std::cout << "Usage: qtllm-mcp-filesystem [--read-root PATH] "
                         "[--write-root PATH]\n";
            return 0;
        }
        const auto writable = arguments[index] == "--write-root";
        if (!writable && arguments[index] != "--read-root")
        {
            std::cerr << "Unknown argument: " << arguments[index] << '\n';
            return 2;
        }
        if (++index >= arguments.size())
        {
            std::cerr << "Missing filesystem root path.\n";
            return 2;
        }
        std::string errorMessage;
        if (!addRoot(pathFromUtf8(arguments[index]), writable, roots,
                     errorMessage))
        {
            std::cerr << errorMessage << '\n';
            return 2;
        }
    }
    if (roots.empty())
    {
        std::cerr << "At least one filesystem root is required.\n";
        return 2;
    }

    std::string line;
    while (std::getline(std::cin, line))
    {
        const auto request = Json::parse(line, nullptr, false);
        if (request.is_discarded() || !request.is_object())
        {
            writeError(nullptr, -32700, "Invalid JSON-RPC message.");
            continue;
        }
        if (!request.contains("id")) continue;
        const auto id = request["id"];
        const auto methodValue = request.find("method");
        const auto method =
            methodValue != request.end() && methodValue->is_string()
                ? methodValue->get<std::string>()
                : std::string{};

        Json result;
        if (method == "initialize")
        {
            auto requestedVersion = std::string("2024-11-05");
            const auto params = request.find("params");
            if (params != request.end() && params->is_object())
            {
                const auto version = params->find("protocolVersion");
                if (version != params->end() && version->is_string())
                    requestedVersion = version->get<std::string>();
            }
            result = {
                {"protocolVersion", requestedVersion},
                {"capabilities", {{"tools", Json::object()}}},
                {"serverInfo",
                 {{"name", "qtllm-mcp-filesystem"}, {"version", "0.2.0"}}}};
        }
        else if (method == "tools/list")
        {
            result = {{"tools", toolDefinitions()}};
        }
        else if (method == "tools/call")
        {
            const auto params = request.find("params");
            if (params == request.end() || !params->is_object())
            {
                writeError(id, -32602, "Invalid tools/call parameters.");
                continue;
            }
            const auto name = params->find("name");
            const auto toolName = name != params->end() && name->is_string()
                                      ? name->get<std::string>()
                                      : std::string{};
            const auto toolArguments = params->find("arguments");
            result = toolResult(callTool(toolName,
                                         toolArguments != params->end()
                                             ? *toolArguments
                                             : Json::object(),
                                         roots));
        }
        else if (method == "ping")
        {
            result = Json::object();
        }
        else
        {
            writeError(id, -32601, "Method not found: " + method);
            continue;
        }
        writeMessage({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
    }
    return 0;
}
}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[])
{
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (auto index = 0; index < argc; ++index)
        arguments.push_back(wideToUtf8(argv[index]));
    return run(arguments);
}
#else
int main(int argc, char* argv[])
{
    return run({argv, argv + argc});
}
#endif
