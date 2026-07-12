// Local HTTP media server for playback tests.
//
// The server binds to loopback, serves static files from a configured root, and
// implements byte-range responses so FFmpeg and browser-like clients can stream
// media. An FTXUI dashboard shows active transfers, aggregate counters, and
// recent request events without scrolling the terminal.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <nlohmann/json.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace ftxui;
using json = nlohmann::json;

// Keeps Winsock initialization tied to process lifetime.
class WinsockGuard {
public:
    WinsockGuard()
    {
        WSADATA data = {};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    }

    ~WinsockGuard()
    {
        WSACleanup();
    }
};

// Move-only RAII wrapper for a Winsock SOCKET.
class Socket {
public:
    explicit Socket(SOCKET socket = INVALID_SOCKET)
        : socket_(socket)
    {
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept
        : socket_(other.socket_)
    {
        other.socket_ = INVALID_SOCKET;
    }

    Socket& operator=(Socket&& other) noexcept
    {
        if (this != &other) {
            close();
            socket_ = other.socket_;
            other.socket_ = INVALID_SOCKET;
        }
        return *this;
    }

    ~Socket()
    {
        close();
    }

    SOCKET get() const
    {
        return socket_;
    }

    bool valid() const
    {
        return socket_ != INVALID_SOCKET;
    }

private:
    void close()
    {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }

    SOCKET socket_ = INVALID_SOCKET;
};

struct Request {
    std::string method;
    std::string target;
    std::optional<std::string> range;
};

struct ByteRange {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
};

struct ServerConfig {
    std::filesystem::path root = std::filesystem::current_path();
    std::uint16_t port = 8080;
    std::map<std::string, std::filesystem::path> files;
};

// Live transfer state shown in the Streams dashboard section.
struct StreamProgress {
    std::uint64_t id = 0;
    std::string method;
    std::string target;
    std::optional<ByteRange> range;
    std::uint64_t bytes_sent = 0;
    std::uint64_t content_length = 0;
    int status = 0;
    bool completed = false;
    bool failed = false;
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
};

// Shared server counters and UI state. Protected fields are grouped here so the
// request workers and UI thread use one lock consistently.
struct ServerStats {
    std::atomic<std::uint64_t> next_request_id = 0;
    std::atomic<std::uint64_t> total_requests = 0;
    std::atomic<std::uint64_t> completed_requests = 0;
    std::atomic<std::uint64_t> failed_requests = 0;
    std::atomic<std::uint64_t> active_requests = 0;
    std::atomic<std::uint64_t> completed_bytes_sent = 0;
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
    std::mutex ui_mutex;
    std::map<std::uint64_t, StreamProgress> streams;
    std::vector<std::string> events;
};

// Tracks active request count through early returns.
struct ActiveRequest {
    explicit ActiveRequest(ServerStats& stats)
        : stats_(stats)
    {
        stats_.active_requests.fetch_add(1);
    }

    ~ActiveRequest()
    {
        stats_.active_requests.fetch_sub(1);
    }

    ServerStats& stats_;
};

// Result summary for one file transfer.
struct TransferResult {
    int status = 200;
    std::uint64_t bytes_sent = 0;
    std::uint64_t content_length = 0;
    std::optional<ByteRange> range;
    bool completed = true;
};

void add_event(const std::shared_ptr<ServerStats>& stats, const std::string& message)
{
    std::lock_guard lock(stats->ui_mutex);
    stats->events.push_back(message);
    if (stats->events.size() > 8) {
        stats->events.erase(stats->events.begin());
    }
}

std::string format_bytes(std::uint64_t bytes)
{
    constexpr double kib = 1024.0;
    constexpr double mib = kib * 1024.0;
    constexpr double gib = mib * 1024.0;

    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(2);
    if (bytes >= static_cast<std::uint64_t>(gib)) {
        stream << static_cast<double>(bytes) / gib << " GiB";
    } else if (bytes >= static_cast<std::uint64_t>(mib)) {
        stream << static_cast<double>(bytes) / mib << " MiB";
    } else if (bytes >= static_cast<std::uint64_t>(kib)) {
        stream << static_cast<double>(bytes) / kib << " KiB";
    } else {
        stream << bytes << " B";
    }
    return stream.str();
}

std::string format_duration(std::int64_t total_seconds)
{
    const std::int64_t days = total_seconds / 86400;
    total_seconds %= 86400;
    const std::int64_t hours = total_seconds / 3600;
    total_seconds %= 3600;
    const std::int64_t minutes = total_seconds / 60;
    const std::int64_t seconds = total_seconds % 60;

    std::ostringstream stream;
    if (days > 0) {
        stream << days << "d ";
    }
    if (days > 0 || hours > 0) {
        stream << hours << "h ";
    }
    if (days > 0 || hours > 0 || minutes > 0) {
        stream << minutes << "m ";
    }
    stream << seconds << "s";
    return stream.str();
}

std::string short_text(std::string text, std::size_t width)
{
    if (text.size() <= width) {
        return text;
    }
    if (width <= 3) {
        return text.substr(0, width);
    }
    return text.substr(0, width - 3) + "...";
}

std::string normalize_route(std::string value)
{
    if (value.empty()) {
        throw std::runtime_error("empty file route");
    }
    if (value.front() != '/') {
        value.insert(value.begin(), '/');
    }
    return value;
}

ServerConfig load_server_config(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("failed to open config file: " + path.string());
    }

    json document;
    file >> document;

    ServerConfig config;
    if (document.contains("root")) {
        config.root = document.at("root").get<std::string>();
    }
    if (document.contains("port")) {
        const int port = document.at("port").get<int>();
        if (port <= 0 || port > 65535) {
            throw std::runtime_error("config port is out of range");
        }
        config.port = static_cast<std::uint16_t>(port);
    }

    if (!document.contains("files") || !document.at("files").is_array()) {
        throw std::runtime_error("config must contain a files array");
    }

    const std::filesystem::path config_base = path.parent_path().empty() ? std::filesystem::current_path() : path.parent_path();
    for (const json& item : document.at("files")) {
        if (!item.contains("route") || !item.contains("path")) {
            throw std::runtime_error("each file entry needs route and path");
        }

        const std::string route = normalize_route(item.at("route").get<std::string>());
        std::filesystem::path media_path = item.at("path").get<std::string>();
        if (media_path.is_relative()) {
            media_path = config_base / media_path;
        }
        config.files[route] = std::filesystem::weakly_canonical(media_path);
    }

    return config;
}

Element dashboard_view(
    const std::shared_ptr<ServerStats>& stats,
    const ServerConfig& config,
    std::uint16_t port)
{
    std::vector<StreamProgress> streams;
    std::vector<std::string> events;
    {
        std::lock_guard lock(stats->ui_mutex);
        for (const auto& [id, stream] : stats->streams) {
            streams.push_back(stream);
        }
        events = stats->events;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - stats->started_at).count();

    Elements stream_rows;
    if (streams.empty()) {
        stream_rows.push_back(text("none") | dim);
    } else {
        for (const StreamProgress& stream : streams) {
            const float ratio = stream.content_length == 0
                ? 0.0
                : static_cast<float>(std::min(1.0, static_cast<double>(stream.bytes_sent) / static_cast<double>(stream.content_length)));
            const double pct = static_cast<double>(ratio) * 100.0;
            const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - stream.started_at).count();
            std::ostringstream title;
            title.setf(std::ios::fixed);
            title.precision(1);
            title << "#" << stream.id
                  << " " << stream.status
                  << " " << pct << "%"
                  << " " << format_bytes(stream.bytes_sent)
                  << " / " << format_bytes(stream.content_length)
                  << " " << age_ms << " ms";
            stream_rows.push_back(vbox({
                hbox({
                    text(title.str()) | size(WIDTH, EQUAL, 46),
                    gauge(ratio) | flex,
                }),
                text(stream.method + " " + short_text(stream.target, 100)) | dim,
            }));
            if (stream.range) {
                stream_rows.push_back(text("range " + std::to_string(stream.range->start) + "-" + std::to_string(stream.range->end)) | dim);
            }
        }
    }

    Elements event_rows;
    if (events.empty()) {
        event_rows.push_back(text("waiting for requests") | dim);
    } else {
        for (const std::string& event : events) {
            event_rows.push_back(text(short_text(event, 110)));
        }
    }

    const auto root = std::filesystem::weakly_canonical(config.root).string();
    return vbox({
               text("Web Video Playback Test Server") | bold,
               separator(),
               hbox(text("Root: "), text(short_text(root, 112))),
               hbox(text("URL:  "), text("http://127.0.0.1:" + std::to_string(port) + "/")),
               hbox(text("Files: "), text(std::to_string(config.files.size()))),
               text("Press Q to stop.") | dim,
               separator(),
               window(text("Streams"), vbox(std::move(stream_rows)) | flex),
               window(
                   text("General"),
                   vbox({
                       text("uptime:                 " + format_duration(uptime)),
                       text("active streams:         " + std::to_string(stats->active_requests.load())),
                       text("started streams:        " + std::to_string(stats->total_requests.load())),
                       text("completed streams:      " + std::to_string(stats->completed_requests.load())),
                       text("failed streams:         " + std::to_string(stats->failed_requests.load())),
                       text("completed stream bytes: " + format_bytes(stats->completed_bytes_sent.load())),
                   })),
               window(text("Output"), vbox(std::move(event_rows))),
           })
        | border;
}

std::string lower_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::optional<std::uint64_t> parse_u64(std::string_view value)
{
    std::uint64_t number = 0;
    const char* first = value.data();
    const char* last = value.data() + value.size();
    const auto result = std::from_chars(first, last, number);
    if (result.ec != std::errc() || result.ptr != last) {
        return std::nullopt;
    }
    return number;
}

std::optional<ByteRange> parse_range(const std::string& header, std::uint64_t file_size)
{
    if (file_size == 0) {
        return std::nullopt;
    }

    constexpr std::string_view prefix = "bytes=";
    if (header.size() <= prefix.size() || header.compare(0, prefix.size(), prefix) != 0) {
        return std::nullopt;
    }

    const std::string_view value(header.data() + prefix.size(), header.size() - prefix.size());
    const std::size_t dash = value.find('-');
    if (dash == std::string_view::npos) {
        return std::nullopt;
    }

    const std::string_view start_text = value.substr(0, dash);
    const std::string_view end_text = value.substr(dash + 1);
    if (start_text.empty() && end_text.empty()) {
        return std::nullopt;
    }

    if (start_text.empty()) {
        const std::optional<std::uint64_t> suffix = parse_u64(end_text);
        if (!suffix || *suffix == 0) {
            return std::nullopt;
        }
        const std::uint64_t length = std::min(*suffix, file_size);
        return ByteRange{file_size - length, file_size - 1};
    }

    const std::optional<std::uint64_t> start = parse_u64(start_text);
    if (!start || *start >= file_size) {
        return std::nullopt;
    }

    std::uint64_t end = file_size - 1;
    if (!end_text.empty()) {
        const std::optional<std::uint64_t> parsed_end = parse_u64(end_text);
        if (!parsed_end || *parsed_end < *start) {
            return std::nullopt;
        }
        end = std::min(*parsed_end, file_size - 1);
    }

    return ByteRange{*start, end};
}

std::string url_decode(std::string_view value)
{
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const std::string_view hex = value.substr(i + 1, 2);
            unsigned int byte = 0;
            std::istringstream stream{std::string(hex)};
            stream >> std::hex >> byte;
            if (!stream.fail()) {
                decoded.push_back(static_cast<char>(byte));
                i += 2;
                continue;
            }
        }
        if (value[i] == '+') {
            decoded.push_back(' ');
        } else {
            decoded.push_back(value[i]);
        }
    }
    return decoded;
}

// Resolves URL paths against either configured file routes or the root.
std::filesystem::path resolve_request_path(const std::filesystem::path& root, std::string target)
{
    const std::size_t query = target.find('?');
    if (query != std::string::npos) {
        target.resize(query);
    }

    target = url_decode(target);
    while (!target.empty() && target.front() == '/') {
        target.erase(target.begin());
    }
    if (target.empty()) {
        target = "index.html";
    }

    const std::filesystem::path root_path = std::filesystem::weakly_canonical(root);
    const std::filesystem::path requested = std::filesystem::weakly_canonical(root_path / std::filesystem::path(target));
    const std::wstring root_text = root_path.native();
    const std::wstring requested_text = requested.native();
    if (requested_text.size() < root_text.size()
        || requested_text.compare(0, root_text.size(), root_text) != 0
        || (requested_text.size() > root_text.size()
            && requested_text[root_text.size()] != L'\\'
            && requested_text[root_text.size()] != L'/')) {
        throw std::runtime_error("path traversal denied");
    }

    return requested;
}

std::filesystem::path resolve_request_path(const ServerConfig& config, std::string target)
{
    const std::size_t query = target.find('?');
    if (query != std::string::npos) {
        target.resize(query);
    }

    target = normalize_route(url_decode(target));
    const auto configured = config.files.find(target);
    if (configured != config.files.end()) {
        return configured->second;
    }

    if (!config.files.empty()) {
        throw std::runtime_error("route is not listed in config");
    }

    return resolve_request_path(config.root, target);
}

std::string content_type(const std::filesystem::path& path)
{
    const std::string extension = lower_copy(path.extension().string());
    if (extension == ".html" || extension == ".htm") return "text/html; charset=utf-8";
    if (extension == ".css") return "text/css; charset=utf-8";
    if (extension == ".js") return "application/javascript; charset=utf-8";
    if (extension == ".json") return "application/json; charset=utf-8";
    if (extension == ".mp4" || extension == ".m4v") return "video/mp4";
    if (extension == ".webm") return "video/webm";
    if (extension == ".mov") return "video/quicktime";
    if (extension == ".mkv") return "video/x-matroska";
    if (extension == ".mp3") return "audio/mpeg";
    if (extension == ".m4a" || extension == ".aac") return "audio/aac";
    if (extension == ".ogg" || extension == ".opus") return "audio/ogg";
    if (extension == ".wav") return "audio/wav";
    return "application/octet-stream";
}

bool send_all(SOCKET socket, const char* data, int size)
{
    int sent = 0;
    while (sent < size) {
        const int result = send(socket, data + sent, size - sent, 0);
        if (result == SOCKET_ERROR || result == 0) {
            return false;
        }
        sent += result;
    }
    return true;
}

bool send_all(SOCKET socket, const std::string& data)
{
    return send_all(socket, data.data(), static_cast<int>(data.size()));
}

void send_status(SOCKET socket, int code, const std::string& text)
{
    std::ostringstream response;
    response << "HTTP/1.1 " << code << ' ' << text << "\r\n"
             << "Content-Length: " << text.size() << "\r\n"
             << "Content-Type: text/plain; charset=utf-8\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "Connection: close\r\n\r\n"
             << text;
    send_all(socket, response.str());
}

std::optional<Request> read_request(SOCKET socket)
{
    std::string raw;
    raw.reserve(4096);
    std::vector<char> buffer(1024);
    while (raw.find("\r\n\r\n") == std::string::npos && raw.size() < 16384) {
        const int received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0) {
            return std::nullopt;
        }
        raw.append(buffer.data(), static_cast<std::size_t>(received));
    }

    std::istringstream stream(raw);
    Request request;
    std::string version;
    stream >> request.method >> request.target >> version;
    if (request.method.empty() || request.target.empty()) {
        return std::nullopt;
    }

    std::string line;
    std::getline(stream, line);
    while (std::getline(stream, line)) {
        if (line == "\r" || line.empty()) {
            break;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string name = lower_copy(trim(std::string_view(line).substr(0, colon)));
        const std::string value = trim(std::string_view(line).substr(colon + 1));
        if (name == "range") {
            request.range = value;
        }
    }

    return request;
}

// Sends either a full response or an HTTP range response.
TransferResult send_file(
    SOCKET socket,
    const Request& request,
    const std::filesystem::path& path,
    const std::shared_ptr<ServerStats>& stats,
    std::uint64_t request_id)
{
    TransferResult transfer;
    const std::uint64_t file_size = std::filesystem::file_size(path);
    const std::optional<ByteRange> range = request.range ? parse_range(*request.range, file_size) : std::nullopt;
    if (request.range && !range) {
        std::ostringstream response;
        response << "HTTP/1.1 416 Range Not Satisfiable\r\n"
                 << "Content-Range: bytes */" << file_size << "\r\n"
                 << "Content-Length: 0\r\n"
                 << "Accept-Ranges: bytes\r\n"
                 << "Access-Control-Allow-Origin: *\r\n"
                 << "Connection: close\r\n\r\n";
        send_all(socket, response.str());
        transfer.status = 416;
        transfer.completed = false;
        return transfer;
    }

    const std::uint64_t start = range ? range->start : 0;
    const std::uint64_t end = range ? range->end : file_size == 0 ? 0 : file_size - 1;
    const std::uint64_t length = file_size == 0 ? 0 : end - start + 1;
    transfer.status = range ? 206 : 200;
    transfer.content_length = length;
    transfer.range = range;
    {
        std::lock_guard lock(stats->ui_mutex);
        auto found = stats->streams.find(request_id);
        if (found != stats->streams.end()) {
            found->second.status = transfer.status;
            found->second.content_length = transfer.content_length;
            found->second.range = transfer.range;
        }
    }

    std::ostringstream header;
    header << "HTTP/1.1 " << (range ? "206 Partial Content" : "200 OK") << "\r\n"
           << "Content-Type: " << content_type(path) << "\r\n"
           << "Content-Length: " << length << "\r\n"
           << "Accept-Ranges: bytes\r\n"
           << "Access-Control-Allow-Origin: *\r\n"
           << "Connection: close\r\n";
    if (range) {
        header << "Content-Range: bytes " << start << '-' << end << '/' << file_size << "\r\n";
    }
    header << "\r\n";
    if (!send_all(socket, header.str()) || request.method == "HEAD") {
        transfer.completed = request.method == "HEAD";
        return transfer;
    }

    std::ifstream file(path, std::ios::binary);
    file.seekg(static_cast<std::streamoff>(start));
    std::vector<char> buffer(64 * 1024);
    std::uint64_t remaining = length;
    while (remaining > 0 && file) {
        const std::streamsize chunk = static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), remaining));
        file.read(buffer.data(), chunk);
        const std::streamsize read = file.gcount();
        if (read <= 0) {
            break;
        }
        if (!send_all(socket, buffer.data(), static_cast<int>(read))) {
            transfer.completed = false;
            break;
        }
        transfer.bytes_sent += static_cast<std::uint64_t>(read);
        {
            std::lock_guard lock(stats->ui_mutex);
            auto found = stats->streams.find(request_id);
            if (found != stats->streams.end()) {
                found->second.bytes_sent = transfer.bytes_sent;
            }
        }
        remaining -= static_cast<std::uint64_t>(read);
    }
    transfer.completed = remaining == 0;
    return transfer;
}

// Handles one client connection. Each accepted socket runs on its own thread.
void handle_client(SOCKET socket, const ServerConfig& config, std::shared_ptr<ServerStats> stats)
{
    const ActiveRequest active(*stats);
    const std::uint64_t request_id = stats->next_request_id.fetch_add(1) + 1;
    stats->total_requests.fetch_add(1);
    const auto start_time = std::chrono::steady_clock::now();

    const std::optional<Request> request = read_request(socket);
    if (!request) {
        send_status(socket, 400, "Bad Request");
        stats->failed_requests.fetch_add(1);
        add_event(stats, "[#" + std::to_string(request_id) + "] 400 Bad Request");
        return;
    }

    {
        std::lock_guard lock(stats->ui_mutex);
        StreamProgress progress;
        progress.id = request_id;
        progress.method = request->method;
        progress.target = request->target;
        stats->streams[request_id] = std::move(progress);
    }

    add_event(stats, "[#" + std::to_string(request_id) + "] " + request->method + " " + request->target);

    if (request->method == "OPTIONS") {
        std::ostringstream response;
        response << "HTTP/1.1 204 No Content\r\n"
                 << "Content-Length: 0\r\n"
                 << "Access-Control-Allow-Origin: *\r\n"
                 << "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
                 << "Access-Control-Allow-Headers: Range\r\n"
                 << "Connection: close\r\n\r\n";
        send_all(socket, response.str());
        stats->completed_requests.fetch_add(1);
        add_event(stats, "[#" + std::to_string(request_id) + "] 204 OPTIONS");
        {
            std::lock_guard lock(stats->ui_mutex);
            stats->streams.erase(request_id);
        }
        return;
    }

    if (request->method != "GET" && request->method != "HEAD") {
        send_status(socket, 405, "Method Not Allowed");
        stats->failed_requests.fetch_add(1);
        add_event(stats, "[#" + std::to_string(request_id) + "] 405 Method Not Allowed");
        {
            std::lock_guard lock(stats->ui_mutex);
            stats->streams.erase(request_id);
        }
        return;
    }

    std::filesystem::path path;
    try {
        path = resolve_request_path(config, request->target);
    } catch (const std::exception&) {
        send_status(socket, 403, "Forbidden");
        stats->failed_requests.fetch_add(1);
        add_event(stats, "[#" + std::to_string(request_id) + "] 403 Forbidden");
        {
            std::lock_guard lock(stats->ui_mutex);
            stats->streams.erase(request_id);
        }
        return;
    }

    if (!std::filesystem::is_regular_file(path)) {
        send_status(socket, 404, "Not Found");
        stats->failed_requests.fetch_add(1);
        add_event(stats, "[#" + std::to_string(request_id) + "] 404 Not Found");
        {
            std::lock_guard lock(stats->ui_mutex);
            stats->streams.erase(request_id);
        }
        return;
    }

    const TransferResult transfer = send_file(socket, *request, path, stats, request_id);
    stats->completed_bytes_sent.fetch_add(transfer.bytes_sent);
    if (transfer.completed && transfer.status < 400) {
        stats->completed_requests.fetch_add(1);
    } else {
        stats->failed_requests.fetch_add(1);
    }

    const auto end_time = std::chrono::steady_clock::now();
    const double elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    std::ostringstream done_log;
    done_log.setf(std::ios::fixed);
    done_log.precision(1);
    done_log << "[#" << request_id << "] " << transfer.status
             << " sent=" << format_bytes(transfer.bytes_sent)
             << " length=" << format_bytes(transfer.content_length)
             << " time=" << elapsed_ms << " ms";
    if (transfer.range) {
        done_log << " content-range=" << transfer.range->start << '-' << transfer.range->end;
    }
    if (!transfer.completed) {
        done_log << " incomplete";
    }
    add_event(stats, done_log.str());
    {
        std::lock_guard lock(stats->ui_mutex);
        auto found = stats->streams.find(request_id);
        if (found != stats->streams.end()) {
            found->second.status = transfer.status;
            found->second.bytes_sent = transfer.bytes_sent;
            found->second.content_length = transfer.content_length;
            found->second.completed = transfer.completed;
            found->second.failed = !transfer.completed || transfer.status >= 400;
            stats->streams.erase(found);
        }
    }
}

ServerConfig parse_args(int argc, char** argv)
{
    ServerConfig config;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--config" && index + 1 < argc) {
            config = load_server_config(argv[++index]);
        } else if (arg == "--root" && index + 1 < argc) {
            config.root = argv[++index];
        } else if (arg == "--port" && index + 1 < argc) {
            const std::optional<std::uint64_t> port = parse_u64(argv[++index]);
            if (!port || *port == 0 || *port > 65535) {
                throw std::runtime_error("invalid port");
            }
            config.port = static_cast<std::uint16_t>(*port);
        } else {
            throw std::runtime_error("usage: webvideoplayback_test_server --config <file> [--port <port>]");
        }
    }
    return config;
}

Socket create_listener(std::uint16_t port)
{
    Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!listener.valid()) {
        throw std::runtime_error("socket failed");
    }

    BOOL reuse = TRUE;
    setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);

    if (bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        throw std::runtime_error("bind failed");
    }
    if (listen(listener.get(), SOMAXCONN) == SOCKET_ERROR) {
        throw std::runtime_error("listen failed");
    }

    return listener;
}

// Owns the UI loop, accept loop, and worker thread lifetime.
void serve(const ServerConfig& config)
{
    const Socket listener = create_listener(config.port);
    std::atomic_bool stop_requested = false;
    auto stats = std::make_shared<ServerStats>();
    auto screen = ScreenInteractive::TerminalOutput();
    auto component = CatchEvent(
        Renderer([&] {
            return dashboard_view(stats, config, config.port);
        }),
        [&](Event event) {
            if (event == Event::Character('q') || event == Event::Character('Q')) {
                stop_requested.store(true);
                screen.ExitLoopClosure()();
                return true;
            }
            return false;
        });

    std::thread refresh_thread([&stop_requested, &screen] {
        while (!stop_requested.load()) {
            screen.PostEvent(Event::Custom);
            Sleep(250);
        }
    });

    std::thread accept_thread([&stop_requested, &listener, config, stats] {
        while (!stop_requested.load()) {
            fd_set read_set = {};
            FD_ZERO(&read_set);
            FD_SET(listener.get(), &read_set);

            timeval timeout = {};
            timeout.tv_sec = 0;
            timeout.tv_usec = 200000;

            const int ready = select(0, &read_set, nullptr, nullptr, &timeout);
            if (ready <= 0) {
                continue;
            }

            sockaddr_in client_address = {};
            int address_length = sizeof(client_address);
            Socket client(accept(listener.get(), reinterpret_cast<sockaddr*>(&client_address), &address_length));
            if (!client.valid()) {
                continue;
            }
            std::thread([client = std::move(client), config, stats] {
                handle_client(client.get(), config, stats);
            }).detach();
        }
    });

    screen.Loop(component);
    stop_requested.store(true);
    if (accept_thread.joinable()) {
        accept_thread.join();
    }
    if (refresh_thread.joinable()) {
        refresh_thread.join();
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const WinsockGuard winsock;
        const ServerConfig config = parse_args(argc, argv);
        serve(config);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
