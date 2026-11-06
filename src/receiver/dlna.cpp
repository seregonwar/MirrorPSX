// SPDX-License-Identifier: GPL-3.0-or-later
// Desktop UPnP AV MediaRenderer: HTTP SOAP, SSDP and bounded GENA
// subscriptions.
#ifdef MIRROR_PS5
#include "platform/ps5_broker.hpp"
#endif
// Winsock types must precede IP Helper declarations.
// clang-format off
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <unistd.h>
using SOCKET = int;
constexpr int INVALID_SOCKET = -1;
using DWORD = unsigned int;
using BOOL = int;
constexpr int TRUE = 1;
inline int closesocket(int s) { return close(s); }
inline void WSACleanup() {}
inline int ioctlsocket(int s, unsigned long cmd, unsigned long* mode) {
  int value = int(*mode);
  return ioctl(s, cmd, &value);
}
#endif
// clang-format on
#include "receiver/dlna.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <libxml/parser.h>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace mirror {
namespace {
#ifdef MIRROR_PS5
constexpr char uuid[] = "uuid:934a93ec-79f5-4ae2-8fa3-4d5053580002";
#else
constexpr char uuid[] = "uuid:934a93ec-79f5-4ae2-8fa3-4d5053580001";
#endif
#ifdef _WIN32
using SocketLength = int;
#else
using SocketLength = socklen_t;
#endif
constexpr char device[] = "urn:schemas-upnp-org:device:MediaRenderer:1";
#ifdef MIRROR_PS5
constexpr char server_identity[] = "OpenProspero/1.0 UPnP/1.0 MirrorPSX/0.1";
constexpr char model_name[] = "MirrorPSX PS5 MediaRenderer";
#else
constexpr char server_identity[] = "Windows/10 UPnP/1.0 MirrorPSX/0.1";
constexpr char model_name[] = "MirrorPSX Desktop MediaRenderer";
#endif
// Android control points drop entries whose cache expires between two
// NOTIFY bursts that Wi-Fi power saving swallowed, so the cache lifetime
// is the common renderer value and announcements repeat well within it.
constexpr int max_age = 1800;
constexpr int announce_seconds = 30;
// A stable port keeps LOCATION valid for phones that cached it before a
// restart; the system picks one only when it is taken.
constexpr unsigned short preferred_http_port = 49595;
const std::array<std::string, 3> services = {"AVTransport", "RenderingControl",
                                             "ConnectionManager"};
std::string type(const std::string &service) {
  return "urn:schemas-upnp-org:service:" + service + ":1";
}
std::string escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '\"':
      out += "&quot;";
      break;
    case '\'':
      out += "&apos;";
      break;
    default:
      out += c;
    }
  }
  return out;
}
std::string lower(std::string s) {
  for (auto &c : s)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string trim(std::string s) {
  auto b = s.find_first_not_of(" \t\r\n\"");
  auto e = s.find_last_not_of(" \t\r\n\"");
  return b == s.npos ? "" : s.substr(b, e - b + 1);
}
bool iequals(const std::string &a, const std::string &b) {
  return a.size() == b.size() && lower(a) == lower(b);
}
bool istarts_with(const std::string &s, const std::string &prefix) {
  return s.size() >= prefix.size() &&
         lower(s.substr(0, prefix.size())) == lower(prefix);
}
std::string http_date() {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  char b[64];
  std::strftime(b, sizeof(b), "%a, %d %b %Y %H:%M:%S GMT", &tm);
  return b;
}
const char *reason(int code) {
  switch (code) {
  case 100:
    return "Continue";
  case 200:
    return "OK";
  case 400:
    return "Bad Request";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 412:
    return "Precondition Failed";
  case 413:
    return "Payload Too Large";
  case 500:
    return "Internal Server Error";
  default:
    return "Error";
  }
}
// Accepts the clock forms sent by Android controllers: H+:MM:SS[.F],
// MM:SS[.F] and plain seconds.
bool parse_clock(const std::string &text, double &seconds) {
  if (text.empty() || text.size() > 32 ||
      text.find_first_not_of("0123456789:.") != text.npos)
    return false;
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    auto colon = text.find(':', start);
    parts.push_back(
        text.substr(start, colon == text.npos ? colon : colon - start));
    if (colon == text.npos)
      break;
    start = colon + 1;
  }
  if (parts.size() > 3)
    return false;
  double total = 0;
  for (size_t i = 0; i < parts.size(); ++i) {
    const auto &part = parts[i];
    const bool last = i + 1 == parts.size();
    if (part.empty() || part.size() > 16 ||
        (!last && part.find('.') != part.npos) ||
        std::count(part.begin(), part.end(), '.') > 1 || part == ".")
      return false;
    double v = std::strtod(part.c_str(), nullptr);
    if (!std::isfinite(v) || v < 0 || (i > 0 && v >= 60))
      return false;
    if (i == 0 && parts.size() == 3 && v > 100000)
      return false;
    total = total * 60 + v;
  }
  seconds = total;
  return std::isfinite(seconds);
}
std::map<std::string, std::string> parse_headers(const std::string &request) {
  std::map<std::string, std::string> headers;
  size_t pos = request.find('\n');
  while (pos != request.npos && ++pos < request.size()) {
    const auto end = request.find('\n', pos);
    const auto line =
        request.substr(pos, end == request.npos ? end : end - pos);
    const auto colon = line.find(':');
    if (colon != line.npos)
      headers[lower(trim(line.substr(0, colon)))] =
          trim(line.substr(colon + 1));
    pos = end;
  }
  return headers;
}
bool send_all(SOCKET socket, const std::string &s) {
  size_t n = 0;
  while (n < s.size()) {
    int r = send(socket, s.data() + n, int(s.size() - n), 0);
    if (r <= 0)
      return false;
    n += r;
  }
  return true;
}
void timeout(SOCKET socket) {
#ifdef _WIN32
  DWORD ms = 1500;
#else
  timeval ms{1, 500000};
#endif
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&ms),
             sizeof(ms));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char *>(&ms),
             sizeof(ms));
}
std::string clock_text(double seconds) {
  int t = int(std::max(0., seconds));
  char b[32];
  std::snprintf(b, sizeof(b), "%02d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
  return b;
}
std::string lan_address() {
#ifdef _WIN32
  ULONG size = 16384;
  std::vector<unsigned char> data(size);
  auto *adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(data.data());
  ULONG r = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr,
                                 adapters, &size);
  if (r == ERROR_BUFFER_OVERFLOW) {
    data.resize(size);
    adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(data.data());
    r = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr,
                             adapters, &size);
  }
  if (r != NO_ERROR)
    return {};
  for (auto *a = adapters; a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp ||
        a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || !a->FirstGatewayAddress)
      continue;
    if (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211)
      continue;
    std::wstring label = a->FriendlyName ? a->FriendlyName : L"";
    if (label.find(L"vEthernet") != label.npos ||
        label.find(L"Virtual") != label.npos)
      continue;
    for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
      if (u->Address.lpSockaddr->sa_family != AF_INET)
        continue;
      char address[INET_ADDRSTRLEN];
      auto *in = reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr);
      if (inet_ntop(AF_INET, &in->sin_addr, address, sizeof(address)))
        return address;
    }
  }
  return {};
#else
  // The multicast route is preferred; a default-route probe covers stacks
  // that report 0.0.0.0 for a connected multicast socket.
  for (const char *target : {"239.255.255.250", "8.8.8.8"}) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET)
      return {};
    sockaddr_in remote{}, local{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(1900);
    inet_pton(AF_INET, target, &remote.sin_addr);
    SocketLength length = sizeof(local);
    char address[INET_ADDRSTRLEN]{};
    const bool ok =
        connect(s, reinterpret_cast<sockaddr *>(&remote), sizeof(remote)) ==
            0 &&
        getsockname(s, reinterpret_cast<sockaddr *>(&local), &length) == 0 &&
        local.sin_addr.s_addr != htonl(INADDR_ANY) &&
        inet_ntop(AF_INET, &local.sin_addr, address, sizeof(address));
    closesocket(s);
    if (ok)
      return address;
  }
  return {};
#endif
}
xmlNode *find(xmlNode *node, const char *name) {
  for (auto *n = node; n; n = n->next) {
    if (n->type == XML_ELEMENT_NODE && xmlStrEqual(n->name, BAD_CAST name))
      return n;
    if (auto *r = find(n->children, name))
      return r;
  }
  return nullptr;
}
std::string value(xmlNode *node, const char *name) {
  auto *n = find(node, name);
  if (!n)
    return {};
  auto *p = xmlNodeGetContent(n);
  std::string s = p ? reinterpret_cast<char *>(p) : "";
  xmlFree(p);
  return s;
}
struct Document {
  xmlDoc *doc = nullptr;
  explicit Document(const std::string &s) {
    if (s.size() <= 65536 && s.find("<!DOCTYPE") == s.npos &&
        s.find("<!ENTITY") == s.npos)
      doc = xmlReadMemory(s.data(), int(s.size()), nullptr, nullptr,
                          XML_PARSE_NONET | XML_PARSE_NOERROR |
                              XML_PARSE_NOWARNING);
  }
  ~Document() { xmlFreeDoc(doc); }
  xmlNode *root() { return doc ? xmlDocGetRootElement(doc) : nullptr; }
};
std::string element(const std::string &name, const std::string &v) {
  return "<" + name + ">" + escape(v) + "</" + name + ">";
}
struct Argument {
  std::string name, direction, variable;
};
using Actions = std::map<std::string, std::vector<Argument>>;
Actions actions(const std::string &s) {
  if (s == "AVTransport")
    return {{"SetAVTransportURI",
             {{"InstanceID", "in", "A_ARG_TYPE_InstanceID"},
              {"CurrentURI", "in", "AVTransportURI"},
              {"CurrentURIMetaData", "in", "AVTransportURIMetaData"}}},
            {"Play",
             {{"InstanceID", "in", "A_ARG_TYPE_InstanceID"},
              {"Speed", "in", "TransportPlaySpeed"}}},
            {"Pause", {{"InstanceID", "in", "A_ARG_TYPE_InstanceID"}}},
            {"Stop", {{"InstanceID", "in", "A_ARG_TYPE_InstanceID"}}},
            {"Seek",
             {{"InstanceID", "in", "A_ARG_TYPE_InstanceID"},
              {"Unit", "in", "A_ARG_TYPE_SeekMode"},
              {"Target", "in", "A_ARG_TYPE_SeekTarget"}}},
            {"GetTransportInfo",
             {{"InstanceID", "in", "A_ARG_TYPE_InstanceID"},
              {"CurrentTransportState", "out", "TransportState"},
              {"CurrentTransportStatus", "out", "TransportStatus"},
              {"CurrentSpeed", "out", "TransportPlaySpeed"}}},
            {"GetPositionInfo",
             {{"InstanceID", "in", "A_ARG_TYPE_InstanceID"},
              {"Track", "out", "CurrentTrack"},
              {"TrackDuration", "out", "CurrentTrackDuration"},
              {"TrackMetaData", "out", "CurrentTrackMetaData"},
              {"TrackURI", "out", "CurrentTrackURI"},
              {"RelTime", "out", "RelativeTimePosition"},
              {"AbsTime", "out", "AbsoluteTimePosition"},
              {"RelCount", "out", "RelativeCounterPosition"},
              {"AbsCount", "out", "AbsoluteCounterPosition"}}},
            {"GetMediaInfo",
             {{"InstanceID", "in", "A_ARG_TYPE_InstanceID"},
              {"NrTracks", "out", "NumberOfTracks"},
              {"MediaDuration", "out", "CurrentMediaDuration"},
              {"CurrentURI", "out", "AVTransportURI"},
              {"CurrentURIMetaData", "out", "AVTransportURIMetaData"},
              {"NextURI", "out", "NextAVTransportURI"},
              {"NextURIMetaData", "out", "NextAVTransportURIMetaData"},
              {"PlayMedium", "out", "PlaybackStorageMedium"},
              {"RecordMedium", "out", "RecordStorageMedium"},
              {"WriteStatus", "out", "RecordMediumWriteStatus"}}},
            {"GetDeviceCapabilities",
             {{"InstanceID", "in", "A_ARG_TYPE_InstanceID"},
              {"PlayMedia", "out", "PossiblePlaybackStorageMedia"},
              {"RecMedia", "out", "PossibleRecordStorageMedia"},
              {"RecQualityModes", "out", "PossibleRecordQualityModes"}}},
            {"GetTransportSettings",
             {{"InstanceID", "in", "A_ARG_TYPE_InstanceID"},
              {"PlayMode", "out", "CurrentPlayMode"},
              {"RecQualityMode", "out", "CurrentRecordQualityMode"}}},
            {"GetCurrentTransportActions",
             {{"InstanceID", "in", "A_ARG_TYPE_InstanceID"},
              {"Actions", "out", "CurrentTransportActions"}}}};
  if (s == "RenderingControl")
    return {{"ListPresets",
             {{"InstanceID", "in", "A_ARG_TYPE_InstanceID"},
              {"CurrentPresetNameList", "out", "PresetNameList"}}},
            {"SelectPreset",
             {{"InstanceID", "in", "A_ARG_TYPE_InstanceID"},
              {"PresetName", "in", "A_ARG_TYPE_PresetName"}}},
            {"GetVolume",
             {{"InstanceID", "in", "A_ARG_TYPE_InstanceID"},
              {"Channel", "in", "A_ARG_TYPE_Channel"},
              {"CurrentVolume", "out", "Volume"}}},
            {"SetVolume",
             {{"InstanceID", "in", "A_ARG_TYPE_InstanceID"},
              {"Channel", "in", "A_ARG_TYPE_Channel"},
              {"DesiredVolume", "in", "Volume"}}},
            {"GetMute",
             {{"InstanceID", "in", "A_ARG_TYPE_InstanceID"},
              {"Channel", "in", "A_ARG_TYPE_Channel"},
              {"CurrentMute", "out", "Mute"}}},
            {"SetMute",
             {{"InstanceID", "in", "A_ARG_TYPE_InstanceID"},
              {"Channel", "in", "A_ARG_TYPE_Channel"},
              {"DesiredMute", "in", "Mute"}}}};
  return {{"GetProtocolInfo",
           {{"Source", "out", "SourceProtocolInfo"},
            {"Sink", "out", "SinkProtocolInfo"}}},
          {"GetCurrentConnectionIDs",
           {{"ConnectionIDs", "out", "CurrentConnectionIDs"}}},
          {"GetCurrentConnectionInfo",
           {{"ConnectionID", "in", "A_ARG_TYPE_ConnectionID"},
            {"RcsID", "out", "A_ARG_TYPE_RcsID"},
            {"AVTransportID", "out", "A_ARG_TYPE_AVTransportID"},
            {"ProtocolInfo", "out", "A_ARG_TYPE_ProtocolInfo"},
            {"PeerConnectionManager", "out", "A_ARG_TYPE_ConnectionManager"},
            {"PeerConnectionID", "out", "A_ARG_TYPE_ConnectionID"},
            {"Direction", "out", "A_ARG_TYPE_Direction"},
            {"Status", "out", "A_ARG_TYPE_ConnectionStatus"}}}};
}
std::string scpd(const std::string &s) {
  auto a = actions(s);
  std::map<std::string, std::string> vars;
  std::string xml = "<?xml version=\"1.0\"?><scpd "
                    "xmlns=\"urn:schemas-upnp-org:service-1-0\"><specVersion><"
                    "major>1</major><minor>0</minor></specVersion><actionList>";
  for (auto &[name, args] : a) {
    xml += "<action>" + element("name", name) + "<argumentList>";
    for (auto &arg : args) {
      xml += "<argument>" + element("name", arg.name) +
             element("direction", arg.direction) +
             element("relatedStateVariable", arg.variable) + "</argument>";
      auto t = "string";
      if (arg.variable == "A_ARG_TYPE_InstanceID" ||
          arg.variable == "CurrentTrack" || arg.variable == "NumberOfTracks")
        t = "ui4";
      if (arg.variable == "RelativeCounterPosition" ||
          arg.variable == "AbsoluteCounterPosition" ||
          arg.variable == "A_ARG_TYPE_ConnectionID" ||
          arg.variable == "A_ARG_TYPE_RcsID" ||
          arg.variable == "A_ARG_TYPE_AVTransportID")
        t = "i4";
      if (arg.variable == "Volume")
        t = "ui2";
      if (arg.variable == "Mute")
        t = "boolean";
      vars[arg.variable] = t;
    }
    xml += "</argumentList></action>";
  }
  xml += "</actionList><serviceStateTable>";
  if (s != "ConnectionManager")
    vars["LastChange"] = "string";
  // Control points such as the jUPnP/Cling based Android apps read these
  // lists to decide which transport features the renderer offers.
  const std::map<std::string, std::vector<std::string>> allowed = {
      {"TransportState",
       {"STOPPED", "PLAYING", "PAUSED_PLAYBACK", "TRANSITIONING",
        "NO_MEDIA_PRESENT"}},
      {"TransportStatus", {"OK", "ERROR_OCCURRED"}},
      {"TransportPlaySpeed", {"1"}},
      {"A_ARG_TYPE_SeekMode", {"REL_TIME", "ABS_TIME"}},
      {"CurrentPlayMode", {"NORMAL"}},
      {"PlaybackStorageMedium", {"NETWORK", "NONE"}},
      {"RecordStorageMedium", {"NOT_IMPLEMENTED"}},
      {"RecordMediumWriteStatus", {"NOT_IMPLEMENTED"}},
      {"CurrentRecordQualityMode", {"NOT_IMPLEMENTED"}},
      {"A_ARG_TYPE_Channel", {"Master"}},
      {"A_ARG_TYPE_PresetName", {"FactoryDefaults"}},
      {"A_ARG_TYPE_Direction", {"Input", "Output"}},
      {"A_ARG_TYPE_ConnectionStatus",
       {"OK", "ContentFormatMismatch", "InsufficientBandwidth",
        "UnreliableChannel", "Unknown"}}};
  const bool connection_manager = s == "ConnectionManager";
  for (auto &[name, t] : vars) {
    const bool evented =
        name == "LastChange" ||
        (connection_manager &&
         (name == "SourceProtocolInfo" || name == "SinkProtocolInfo" ||
          name == "CurrentConnectionIDs"));
    xml += "<stateVariable sendEvents=\"" +
           std::string(evented ? "yes" : "no") + "\">" + element("name", name) +
           element("dataType", t);
    if (auto it = allowed.find(name); it != allowed.end()) {
      xml += "<allowedValueList>";
      for (auto &v : it->second)
        xml += element("allowedValue", v);
      xml += "</allowedValueList>";
    }
    if (name == "Volume")
      xml += "<allowedValueRange><minimum>0</minimum><maximum>100</"
             "maximum><step>1</step></allowedValueRange>";
    xml += "</stateVariable>";
  }
  return xml + "</serviceStateTable></scpd>";
}
// Android senders filter renderers by MIME before offering them, and
// several report the same container under different names.
constexpr char protocols[] =
    "http-get:*:video/mp4:*,http-get:*:video/x-m4v:*,http-get:*:video/"
    "x-matroska:*,http-get:*:video/x-mkv:*,http-get:*:video/webm:*,http-"
    "get:*:video/quicktime:*,http-get:*:video/x-msvideo:*,http-get:*:video/"
    "avi:*,http-get:*:video/3gpp:*,http-get:*:video/x-flv:*,http-get:*:"
    "video/mpeg:*,http-get:*:video/MP2T:*,http-get:*:video/"
    "vnd.dlna.mpeg-tts:*,http-get:*:application/vnd.apple.mpegurl:*,http-"
    "get:*:application/x-mpegURL:*";
} // namespace
struct DlnaReceiver::Impl {
  State &state;
  MediaPlayer &player;
  SOCKET http = INVALID_SOCKET, ssdp = INVALID_SOCKET;
  std::atomic<bool> running{false};
  // SSDP has its own thread: a slow HTTP client must never hold a search
  // reply past the sender's MX window (Android apps listen ~3 s).
  std::thread worker, discovery;
  std::string ip, location, metadata;
  struct Subscription {
    std::string sid, service, path, host;
    sockaddr_in address{};
    uint32_t sequence = 0;
    std::chrono::steady_clock::time_point expiry;
  };
  std::vector<Subscription> subscriptions;
  uint64_t next_sid = 0;
  Impl(State &s, MediaPlayer &p) : state(s), player(p) {}
  ~Impl() { stop(); }
  std::vector<std::string> targets() {
    std::vector<std::string> t = {"upnp:rootdevice", uuid, device};
    for (auto &s : services)
      t.push_back(type(s));
    return t;
  }
  void announce(bool alive) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    for (auto &t : targets()) {
      std::string usn = std::string(uuid) + (t == uuid ? "" : "::" + t);
      auto message =
          "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nNT: " + t +
          "\r\nNTS: ssdp:" + (alive ? "alive" : "byebye") + "\r\nUSN: " + usn +
          (alive ? "\r\nCACHE-CONTROL: max-age=" + std::to_string(max_age) +
                       "\r\nLOCATION: " + location +
                       "\r\nSERVER: " + server_identity
                 : std::string()) +
          "\r\n\r\n";
      sendto(ssdp, message.data(), int(message.size()), 0,
             reinterpret_cast<sockaddr *>(&to), sizeof(to));
    }
  }
  // UDP multicast over Wi-Fi is lossy; the UPnP spec allows repeating
  // each announcement and phones in power saving often miss a single one.
  void announce_burst(bool alive, int count) {
    for (int i = 0; i < count; ++i) {
      if (i)
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
      announce(alive);
    }
  }
  bool start() {
    stop();
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
      return false;
#endif
    ip = lan_address();
    if (ip.empty()) {
      WSACleanup();
      return false;
    }
    http = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ssdp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (http == INVALID_SOCKET || ssdp == INVALID_SOCKET) {
      cleanup();
      return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip.c_str(), &address.sin_addr);
    address.sin_port = htons(preferred_http_port);
    if (bind(http, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
      address.sin_port = 0;
      if (bind(http, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
        cleanup();
        return false;
      }
    }
    if (listen(http, 16)) {
      cleanup();
      return false;
    }
    SocketLength size = sizeof(address);
    getsockname(http, reinterpret_cast<sockaddr *>(&address), &size);
    location = "http://" + ip + ":" + std::to_string(ntohs(address.sin_port)) +
               "/device.xml";
    BOOL reuse = TRUE;
    setsockopt(ssdp, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char *>(&reuse),
               sizeof(reuse));
#ifdef SO_REUSEPORT
    // BSD stacks (PS5) deliver multicast to every socket on 1900 only with
    // SO_REUSEPORT, so another UPnP app on the console does not block us.
    setsockopt(ssdp, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<char *>(&reuse),
               sizeof(reuse));
#endif
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(1900);
    if (bind(ssdp, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
      cleanup();
      return false;
    }
    ip_mreq membership{};
    inet_pton(AF_INET, "239.255.255.250", &membership.imr_multiaddr);
    inet_pton(AF_INET, ip.c_str(), &membership.imr_interface);
    if (setsockopt(ssdp, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   reinterpret_cast<char *>(&membership), sizeof(membership))) {
      cleanup();
      return false;
    }
    setsockopt(ssdp, IPPROTO_IP, IP_MULTICAST_IF,
               reinterpret_cast<char *>(&membership.imr_interface),
               sizeof(membership.imr_interface));
    DWORD ttl = 4;
    setsockopt(ssdp, IPPROTO_IP, IP_MULTICAST_TTL,
               reinterpret_cast<char *>(&ttl), sizeof(ttl));
    running = true;
    state.update([&](auto &s) {
      s.listening = true;
      s.name = "MirrorPSX - Video";
      s.media_address = location;
      s.error.clear();
    });
    worker = std::thread([this] { run(); });
    discovery = std::thread([this] { ssdp_loop(); });
    std::fprintf(stderr, "[MirrorPSX] DLNA ready: %s\n", location.c_str());
    return true;
  }
  void cleanup() {
    if (http != INVALID_SOCKET)
      closesocket(http);
    if (ssdp != INVALID_SOCKET)
      closesocket(ssdp);
    http = ssdp = INVALID_SOCKET;
    subscriptions.clear();
    WSACleanup();
  }
  void stop() {
    if (!running.exchange(false))
      return;
    if (worker.joinable())
      worker.join();
    if (discovery.joinable())
      discovery.join();
    announce_burst(false, 2);
    cleanup();
    player.stop();
    state.update([](auto &s) {
      s.listening = false;
      s.media_address.clear();
    });
  }
  std::string description() {
    const std::string base = location.substr(0, location.rfind('/') + 1);
    // The DLNA document version and the descriptive fields are what Android
    // casting stacks (vendor gallery/video apps, jUPnP) use to classify a
    // device as a Digital Media Renderer and list it.
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><root "
        "xmlns=\"urn:schemas-upnp-org:device-1-0\" "
        "xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\"><specVersion><"
        "major>1</major><minor>0</minor></specVersion>";
    xml += element("URLBase", base) + "<device>";
    xml +=
        element("deviceType", device) +
        element("friendlyName", "MirrorPSX - Video") +
        element("manufacturer", "MirrorPSX") +
        element("manufacturerURL", "https://github.com/seregonwar/MirrorPSX") +
        element("modelDescription", "MirrorPSX UPnP AV MediaRenderer") +
        element("modelName", model_name) + element("modelNumber", "0.1") +
        element("modelURL", "https://github.com/seregonwar/MirrorPSX") +
        element("serialNumber", std::string(uuid).substr(5)) +
        element("UDN", uuid) +
        "<dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC><serviceList>";
    for (auto &s : services)
      xml += "<service>" + element("serviceType", type(s)) +
             element("serviceId", "urn:upnp-org:serviceId:" + s) +
             element("SCPDURL", "/" + s + "/scpd.xml") +
             element("controlURL", "/" + s + "/control") +
             element("eventSubURL", "/" + s + "/event") + "</service>";
    return xml + "</serviceList></device></root>";
  }
  void response(SOCKET client, int code, const std::string &body,
                const std::string &headers = "", bool head = false) {
    send_all(client, "HTTP/1.1 " + std::to_string(code) + " " + reason(code) +
                         "\r\nContent-Type: text/xml; "
                         "charset=\"utf-8\"\r\nContent-Length: " +
                         std::to_string(body.size()) + "\r\nDate: " +
                         http_date() + "\r\nServer: " + server_identity +
                         "\r\nConnection: close\r\n" + headers + "\r\n" +
                         (head ? std::string() : body));
  }
  std::string event(const std::string &service) {
    auto s = player.status();
    std::string values;
    if (service == "AVTransport")
      values = "<TransportState val=\"" + s.transport +
               "\"/><TransportStatus val=\"" +
               (s.error.empty() ? "OK" : "ERROR_OCCURRED") +
               "\"/><AVTransportURI val=\"" + escape(s.uri) +
               "\"/><CurrentTrackDuration val=\"" + clock_text(s.duration) +
               "\"/><CurrentTransportActions val=\"Play,Pause,Stop,Seek\"/>";
    else
      values = "<Volume channel=\"Master\" val=\"" + std::to_string(s.volume) +
               "\"/><Mute channel=\"Master\" val=\"" + (s.muted ? "1" : "0") +
               "\"/>";
    std::string change = "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/" +
                         std::string(service == "AVTransport" ? "AVT" : "RCS") +
                         "/\"><InstanceID val=\"0\">" + values +
                         "</InstanceID></Event>";
    return "<e:propertyset "
           "xmlns:e=\"urn:schemas-upnp-org:event-1-0\"><e:property>" +
           (service == "ConnectionManager"
                ? element("SinkProtocolInfo", protocols)
                : element("LastChange", change)) +
           "</e:property></e:propertyset>";
  }
  void notify(Subscription &s) {
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET)
      return;
    timeout(client);
    // Nonblocking connect bounds unreachable subscriber waits as well.
    u_long nonblocking = 1;
    ioctlsocket(client, FIONBIO, &nonblocking);
    int result = connect(client, reinterpret_cast<sockaddr *>(&s.address),
                         sizeof(s.address));
    if (result != 0) {
      fd_set set;
      FD_ZERO(&set);
      FD_SET(client, &set);
      timeval tv{0, 200000};
      result =
          select(int(client) + 1, nullptr, &set, nullptr, &tv) > 0 ? 0 : -1;
    }
    nonblocking = 0;
    ioctlsocket(client, FIONBIO, &nonblocking);
    if (result == 0) {
      auto body = event(s.service);
      send_all(client,
               "NOTIFY " + s.path + " HTTP/1.1\r\nHOST: " + s.host +
                   "\r\nCONTENT-TYPE: text/xml\r\nNT: upnp:event\r\nNTS: "
                   "upnp:propchange\r\nSID: " +
                   s.sid + "\r\nSEQ: " + std::to_string(s.sequence++) +
                   "\r\nCONTENT-LENGTH: " + std::to_string(body.size()) +
                   "\r\nConnection: close\r\n\r\n" + body);
    }
    closesocket(client);
  }
  void soap(SOCKET client, const std::string &service, const std::string &body,
            const std::string &soap_action) {
    Document doc(body);
    auto *root = doc.root();
    auto *b = find(root, "Body");
    auto *a = b ? b->children : nullptr;
    while (a && a->type != XML_ELEMENT_NODE)
      a = a->next;
    int fault = 0;
    std::string output,
        action = a ? reinterpret_cast<const char *>(a->name) : "";
    // Some Android senders omit the action namespace or use a higher service
    // version; the SOAPACTION header then identifies the service.
    const std::string family = "urn:schemas-upnp-org:service:" + service + ":";
    const std::string href = a && a->ns && a->ns->href
                                 ? reinterpret_cast<const char *>(a->ns->href)
                                 : "";
    const bool known_service = href.empty() ? istarts_with(soap_action, family)
                                            : istarts_with(href, family);
    const auto instance = trim(value(a, "InstanceID"));
    if (!a || !known_service || !actions(service).contains(action))
      fault = 401;
    else if (service != "ConnectionManager" && instance != "0" &&
             !instance.empty())
      fault = 718;
    else {
      auto s = player.status();
      auto add = [&](const char *n, const std::string &v) {
        output += element(n, v);
      };
      if (action == "SetAVTransportURI") {
        std::string uri = value(a, "CurrentURI"),
                    meta = value(a, "CurrentURIMetaData");
        Document d(meta);
        if (!player.load(uri, value(d.root(), "title")))
          fault = 714;
        else
          metadata = meta;
      } else if (action == "Play") {
        const auto speed = trim(value(a, "Speed"));
        if (!speed.empty() && speed != "1" && speed != "1.0" && speed != "1/1")
          fault = 717;
        else if (s.uri.empty())
          fault = 701;
        else
          player.play();
      } else if (action == "Pause")
        player.pause();
      else if (action == "Stop")
        player.stop();
      else if (action == "Seek") {
        // A single-track renderer has identical relative and absolute time;
        // several Android apps only send ABS_TIME.
        const auto unit = trim(value(a, "Unit"));
        const auto target = trim(value(a, "Target"));
        double seconds = 0;
        if (unit != "REL_TIME" && unit != "ABS_TIME")
          fault = 710;
        else if (!parse_clock(target, seconds) || !player.seek(seconds))
          fault = 711;
      } else if (action == "GetTransportInfo") {
        add("CurrentTransportState", s.transport);
        add("CurrentTransportStatus",
            s.error.empty() ? "OK" : "ERROR_OCCURRED");
        add("CurrentSpeed", "1");
      } else if (action == "GetPositionInfo") {
        add("Track", s.uri.empty() ? "0" : "1");
        add("TrackDuration", clock_text(s.duration));
        add("TrackMetaData", metadata);
        add("TrackURI", s.uri);
        add("RelTime", clock_text(s.position));
        add("AbsTime", clock_text(s.position));
        add("RelCount", "2147483647");
        add("AbsCount", "2147483647");
      } else if (action == "GetMediaInfo") {
        add("NrTracks", s.uri.empty() ? "0" : "1");
        add("MediaDuration", clock_text(s.duration));
        add("CurrentURI", s.uri);
        add("CurrentURIMetaData", metadata);
        add("NextURI", "");
        add("NextURIMetaData", "");
        add("PlayMedium", "NETWORK");
        add("RecordMedium", "NOT_IMPLEMENTED");
        add("WriteStatus", "NOT_IMPLEMENTED");
      } else if (action == "GetDeviceCapabilities") {
        add("PlayMedia", "NETWORK");
        add("RecMedia", "NOT_IMPLEMENTED");
        add("RecQualityModes", "NOT_IMPLEMENTED");
      } else if (action == "GetTransportSettings") {
        add("PlayMode", "NORMAL");
        add("RecQualityMode", "NOT_IMPLEMENTED");
      } else if (action == "GetCurrentTransportActions")
        add("Actions", "Play,Pause,Stop,Seek");
      else if (action == "GetProtocolInfo") {
        add("Source", "");
        add("Sink", protocols);
      } else if (action == "GetCurrentConnectionIDs")
        add("ConnectionIDs", "0");
      else if (action == "GetCurrentConnectionInfo") {
        if (value(a, "ConnectionID") != "0")
          fault = 706;
        else {
          add("RcsID", "0");
          add("AVTransportID", "0");
          add("ProtocolInfo", protocols);
          add("PeerConnectionManager", "");
          add("PeerConnectionID", "-1");
          add("Direction", "Input");
          add("Status", "OK");
        }
      } else if (action == "ListPresets")
        add("CurrentPresetNameList", "FactoryDefaults");
      else if (action == "SelectPreset") {
        if (value(a, "PresetName") != "FactoryDefaults")
          fault = 701;
        else {
          player.volume(100);
          player.mute(false);
        }
      } else if (auto channel = trim(value(a, "Channel"));
                 !channel.empty() && !iequals(channel, "Master"))
        fault = 402;
      else if (action == "GetVolume")
        add("CurrentVolume", std::to_string(s.volume));
      else if (action == "GetMute")
        add("CurrentMute", s.muted ? "1" : "0");
      else if (action == "SetVolume") {
        auto v = trim(value(a, "DesiredVolume"));
        if (v.empty() || v.size() > 3 ||
            v.find_first_not_of("0123456789") != v.npos || std::stoi(v) > 100)
          fault = 402;
        else
          player.volume(std::stoi(v));
      } else if (action == "SetMute") {
        auto v = lower(trim(value(a, "DesiredMute")));
        if (v == "true")
          v = "1";
        else if (v == "false")
          v = "0";
        if (v != "0" && v != "1")
          fault = 402;
        else
          player.mute(v == "1");
      }
    }
    std::string result =
        fault
            ? "<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</"
              "faultstring><detail><UPnPError "
              "xmlns=\"urn:schemas-upnp-org:control-1-0\">" +
                  element("errorCode", std::to_string(fault)) +
                  element("errorDescription", "Action failed") +
                  "</UPnPError></detail></s:Fault>"
            : "<u:" + action + "Response xmlns:u=\"" + type(service) + "\">" +
                  output + "</u:" + action + "Response>";
    response(client, fault ? 500 : 200,
             "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope "
             "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/"
             "\"><s:Body>" +
                 result + "</s:Body></s:Envelope>",
             "EXT:\r\n");
  }
  void handle(SOCKET client, sockaddr_in peer) {
    timeout(client);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    std::string request;
    char buffer[4096];
    size_t end;
    while ((end = request.find("\r\n\r\n")) == request.npos) {
      if (!running || std::chrono::steady_clock::now() > deadline)
        return;
      int n = recv(client, buffer, sizeof(buffer), 0);
      if (n <= 0)
        return;
      request.append(buffer, n);
      if (request.size() > 16384) {
        response(client, 413, "");
        return;
      }
    }
    std::string method, path, version;
    const auto first = request.find(' '), second = request.find(' ', first + 1);
    if (first == request.npos || second == request.npos || second > end) {
      response(client, 400, "");
      return;
    }
    method = request.substr(0, first);
    path = request.substr(first + 1, second - first - 1);
    version = trim(request.substr(second + 1, request.find('\n') - second - 1));
    auto headers = parse_headers(request.substr(0, end));
    // Absolute request targets and query strings come from some Java HTTP
    // stacks; routing only looks at the path.
    if (istarts_with(path, "http://")) {
      auto slash = path.find('/', 7);
      path = slash == path.npos ? "/" : path.substr(slash);
    }
    if (auto q = path.find('?'); q != path.npos)
      path.resize(q);
    const bool head = method == "HEAD";
    if (head)
      method = "GET";
    const auto encoding = headers.find("transfer-encoding");
    const bool chunked =
        encoding != headers.end() &&
        lower(encoding->second).find("chunked") != std::string::npos;
    if (encoding != headers.end() && !chunked) {
      response(client, 400, "");
      return;
    }
    size_t length = 0;
    auto len = headers["content-length"];
    if (!len.empty() && !chunked) {
      if (len.size() > 6 || len.find_first_not_of("0123456789") != len.npos) {
        response(client, 400, "");
        return;
      }
      length = std::stoul(len);
    }
    if (length > 65536) {
      response(client, 413, "");
      return;
    }
    // Apache/Java clients wait for this before sending a SOAP body.
    if (lower(headers["expect"]) == "100-continue")
      send_all(client, "HTTP/1.1 100 Continue\r\n\r\n");
    std::string body = request.substr(end + 4);
    auto receive = [&]() {
      if (!running || std::chrono::steady_clock::now() > deadline)
        return false;
      int n = recv(client, buffer, sizeof(buffer), 0);
      if (n <= 0)
        return false;
      body.append(buffer, n);
      return body.size() <= 65536 + 4096;
    };
    if (chunked) {
      std::string decoded;
      size_t pos = 0;
      for (;;) {
        size_t line;
        while ((line = body.find("\r\n", pos)) == body.npos)
          if (!receive())
            return;
        auto size_text = trim(body.substr(pos, line - pos));
        if (auto semi = size_text.find(';'); semi != size_text.npos)
          size_text = trim(size_text.substr(0, semi));
        if (size_text.empty() || size_text.size() > 6 ||
            size_text.find_first_not_of("0123456789abcdefABCDEF") !=
                size_text.npos) {
          response(client, 400, "");
          return;
        }
        const size_t chunk = std::stoul(size_text, nullptr, 16);
        if (decoded.size() + chunk > 65536) {
          response(client, 413, "");
          return;
        }
        pos = line + 2;
        if (chunk == 0)
          break;
        while (body.size() < pos + chunk + 2)
          if (!receive())
            return;
        decoded.append(body, pos, chunk);
        pos += chunk + 2;
      }
      body = std::move(decoded);
    } else {
      while (body.size() < length) {
        if (!running || std::chrono::steady_clock::now() > deadline)
          return;
        int n = recv(client, buffer,
                     int(std::min(sizeof(buffer), length - body.size())), 0);
        if (n <= 0)
          return;
        body.append(buffer, n);
      }
      body.resize(length);
    }
    if (method == "GET" && (path == "/device.xml" || path == "/")) {
      response(client, 200, description(), "", head);
      return;
    }
    for (auto &s : services) {
      if (path == "/" + s + "/scpd.xml" && method == "GET") {
        response(client, 200, scpd(s), "", head);
        return;
      }
      if (path == "/" + s + "/control" && method == "POST") {
        soap(client, s, body, trim(headers["soapaction"]));
        return;
      }
      if (path == "/" + s + "/event") {
        auto sid = headers["sid"];
        auto it = std::find_if(
            subscriptions.begin(), subscriptions.end(), [&](auto &e) {
              return e.sid == sid && e.service == s &&
                     e.address.sin_addr.s_addr == peer.sin_addr.s_addr;
            });
        if (method == "UNSUBSCRIBE") {
          if (it == subscriptions.end()) {
            response(client, 412, "");
            return;
          }
          subscriptions.erase(it);
          response(client, 200, "");
          return;
        }
        if (method != "SUBSCRIBE") {
          response(client, 405, "");
          return;
        }
        if (sid.empty()) {
          auto cb = headers["callback"];
          if (subscriptions.size() >= 16 || headers["nt"] != "upnp:event" ||
              cb.rfind("<http://", 0) != 0 || cb.back() != '>') {
            response(client, 412, "");
            return;
          }
          cb = cb.substr(8, cb.size() - 9);
          auto slash = cb.find('/');
          auto colon = cb.find(':');
          if (slash == cb.npos || colon == cb.npos || colon > slash) {
            response(client, 412, "");
            return;
          }
          sockaddr_in address = peer;
          std::string host = cb.substr(0, colon),
                      port = cb.substr(colon + 1, slash - colon - 1);
          in_addr addr{};
          if (inet_pton(AF_INET, host.c_str(), &addr) != 1 ||
              addr.s_addr != peer.sin_addr.s_addr || port.empty() ||
              port.size() > 5 ||
              port.find_first_not_of("0123456789") != port.npos ||
              std::stoi(port) < 1 || std::stoi(port) > 65535) {
            response(client, 412, "");
            return;
          }
          address.sin_port = htons(std::stoi(port));
          subscriptions.push_back(
              {std::string(uuid) + "-" + std::to_string(++next_sid), s,
               cb.substr(slash), cb.substr(0, slash), address});
          it = std::prev(subscriptions.end());
        } else if (it == subscriptions.end()) {
          response(client, 412, "");
          return;
        }
        // Honour shorter requested lifetimes; cap long and infinite ones.
        int lifetime = 1800;
        auto requested = lower(headers["timeout"]);
        if (requested.rfind("second-", 0) == 0) {
          auto digits = requested.substr(7);
          if (!digits.empty() && digits.size() <= 6 &&
              digits.find_first_not_of("0123456789") == digits.npos)
            lifetime = std::clamp(std::stoi(digits), 60, 1800);
        }
        it->expiry =
            std::chrono::steady_clock::now() + std::chrono::seconds(lifetime);
        response(client, 200, "",
                 "SID: " + it->sid + "\r\nTIMEOUT: Second-" +
                     std::to_string(lifetime) + "\r\n");
        notify(*it);
        return;
      }
    }
    response(client, 404, "");
  }
  void discover() {
    char buffer[4096];
    sockaddr_in peer{};
    SocketLength size = sizeof(peer);
    int n = recvfrom(ssdp, buffer, sizeof(buffer), 0,
                     reinterpret_cast<sockaddr *>(&peer), &size);
    if (n <= 0)
      return;
    std::string message(buffer, n);
    // Header names and the request line are case-insensitive in practice;
    // several Android SSDP stacks send "M-Search" or lower-case headers.
    if (!istarts_with(message, "M-SEARCH * HTTP/1.") ||
        lower(message).find("ssdp:discover") == message.npos)
      return;
    auto headers = parse_headers(message);
    const auto st = trim(headers["st"]);
    for (auto &t : targets()) {
      if (!iequals(st, "ssdp:all") && !iequals(st, t))
        continue;
      auto reply = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=" +
                   std::to_string(max_age) + "\r\nDATE: " + http_date() +
                   "\r\nEXT:\r\nLOCATION: " + location +
                   "\r\nSERVER: " + server_identity + "\r\nST: " + t +
                   "\r\nUSN: " + uuid + (t == uuid ? "" : "::" + t) +
                   "\r\nContent-Length: 0\r\n\r\n";
      sendto(ssdp, reply.data(), int(reply.size()), 0,
             reinterpret_cast<sockaddr *>(&peer), sizeof(peer));
    }
  }
  void ssdp_loop() {
    // Clear stale cache entries from a previous run, then announce.
    announce(false);
    announce_burst(true, 3);
    auto announce_at = std::chrono::steady_clock::now();
    while (running) {
      fd_set set;
      FD_ZERO(&set);
      FD_SET(ssdp, &set);
      timeval tv{0, 200000};
      if (select(int(ssdp) + 1, &set, nullptr, nullptr, &tv) > 0 &&
          FD_ISSET(ssdp, &set))
        discover();
      auto now = std::chrono::steady_clock::now();
      if (now - announce_at > std::chrono::seconds(announce_seconds)) {
        announce_burst(true, 2);
        announce_at = now;
      }
    }
  }
  void run() {
    auto event_at = std::chrono::steady_clock::now();
    std::string previous;
    while (running) {
      fd_set set;
      FD_ZERO(&set);
      FD_SET(http, &set);
      timeval tv{0, 100000};
      if (select(int(http) + 1, &set, nullptr, nullptr, &tv) > 0) {
        if (FD_ISSET(http, &set)) {
          sockaddr_in peer{};
          SocketLength size = sizeof(peer);
          auto client =
              accept(http, reinterpret_cast<sockaddr *>(&peer), &size);
          if (client != INVALID_SOCKET) {
            handle(client, peer);
            closesocket(client);
          }
        }
      }
      auto now = std::chrono::steady_clock::now();
      if (now - event_at > std::chrono::milliseconds(500)) {
        auto status = player.status();
        std::string current = status.transport + status.error + status.uri +
                              std::to_string(status.volume) +
                              std::to_string(status.muted) +
                              clock_text(status.duration);
        std::erase_if(subscriptions, [&](auto &s) { return s.expiry < now; });
        if (current != previous) {
          for (auto &s : subscriptions)
            notify(s);
          previous = current;
        }
        event_at = now;
      }
    }
  }
};
DlnaReceiver::DlnaReceiver(State &s, MediaPlayer &p)
    : impl_(std::make_unique<Impl>(s, p)) {}
DlnaReceiver::~DlnaReceiver() = default;
bool DlnaReceiver::start() {
#ifdef MIRROR_PS5
  std::string broker_error;
  if (!prepare_ps5_network(broker_error)) {
    impl_->state.update([&](auto &s) { s.error = broker_error; });
    return false;
  }
#endif
  if (impl_->start())
    return true;
  impl_->state.update([](auto &s) {
    s.error = "Avvio DLNA fallito. Verifica la rete locale.";
  });
  return false;
}
void DlnaReceiver::stop() { impl_->stop(); }
} // namespace mirror
