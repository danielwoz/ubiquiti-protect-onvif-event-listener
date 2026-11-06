// Copyright 2026 Daniel W
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "onvif_camera_emulator.hpp"

#include <openssl/evp.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>

// ============================================================
// Construction / lifecycle
// ============================================================
OnvifCameraEmulator::OnvifCameraEmulator(const std::string& real_ip)
    : real_ip_(real_ip) {}

OnvifCameraEmulator::~OnvifCameraEmulator() {
    stop();
}

void OnvifCameraEmulator::start() {
    daemon_ = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_THREAD_PER_CONNECTION,
        0,              // port 0 → OS picks a free port
        nullptr,        // accept policy callback
        nullptr,        // accept policy userdata
        &on_request,    // request handler
        this,           // userdata for handler
        MHD_OPTION_NOTIFY_COMPLETED, &on_completed, this,
        MHD_OPTION_END);

    if (!daemon_) {
        std::fprintf(stderr, "Fatal: Failed to start MHD daemon for %s\n",
                     real_ip_.c_str());
        std::abort();
    }

    const auto* info = MHD_get_daemon_info(daemon_, MHD_DAEMON_INFO_BIND_PORT);
    if (!info) {
        std::fprintf(stderr, "Fatal: Failed to query daemon port for %s\n",
                     real_ip_.c_str());
        std::abort();
    }

    port_ = info->port;
}

void OnvifCameraEmulator::stop() {
    if (daemon_) {
        MHD_stop_daemon(daemon_);
        daemon_ = nullptr;
    }
}

std::string OnvifCameraEmulator::local_address() const {
    return "127.0.0.1:" + std::to_string(port_);
}

// ============================================================
// URL rewriting
// ============================================================
std::string OnvifCameraEmulator::rewrite_urls(const std::string& resp) const {
    const std::string local = "http://127.0.0.1:" + std::to_string(port_) + "/";
    std::string out = resp;

    for (const std::string& old : {
            "http://" + real_ip_ + "/",
            "http://" + real_ip_ + ":80/" }) {
        size_t pos = 0;
        while ((pos = out.find(old, pos)) != std::string::npos) {
            out.replace(pos, old.size(), local);
            pos += local.size();
        }
    }
    return out;
}

// ============================================================
// libmicrohttpd callbacks
// ============================================================
// Connection of the request currently being dispatched on this thread.
static struct MHD_Connection*& current_connection() {
    thread_local struct MHD_Connection* conn = nullptr;
    return conn;
}

std::string OnvifCameraEmulator::request_header(const char* name) {
    if (current_connection() == nullptr) return {};
    const char* v = MHD_lookup_connection_value(current_connection(),
                                                MHD_HEADER_KIND, name);
    return v ? v : "";
}

// ============================================================
// HTTP Digest (RFC 7616, qop=auth)
// ============================================================
namespace {

std::string md5_hex(const std::string& text) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    EVP_MD_CTX*   ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
    EVP_DigestUpdate(ctx, text.data(), text.size());
    EVP_DigestFinal_ex(ctx, digest, &len);
    EVP_MD_CTX_free(ctx);
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

// Value of parameter @p key in an "Authorization: Digest ..." header, or
// empty when absent.
std::string digest_param(const std::string& hdr, const std::string& key) {
    std::size_t p = hdr.find(' ');
    if (p == std::string::npos) return {};
    p += 1;
    while (p < hdr.size()) {
        while (p < hdr.size() && (hdr[p] == ' ' || hdr[p] == ',')) ++p;
        const std::size_t name_start = p;
        while (p < hdr.size() && hdr[p] != '=' && hdr[p] != ',') ++p;
        const std::string name = hdr.substr(name_start, p - name_start);
        if (p >= hdr.size() || hdr[p] != '=') return {};
        ++p;
        std::string value;
        if (p < hdr.size() && hdr[p] == '"') {
            ++p;
            const std::size_t start = p;
            while (p < hdr.size() && hdr[p] != '"') ++p;
            value = hdr.substr(start, p - start);
            if (p < hdr.size()) ++p;
        } else {
            const std::size_t start = p;
            while (p < hdr.size() && hdr[p] != ',') ++p;
            value = hdr.substr(start, p - start);
        }
        if (name == key) return value;
    }
    return {};
}

}  // namespace

void OnvifCameraEmulator::require_http_digest(const std::string& user,
                                              const std::string& password) {
    digest_required_ = true;
    digest_user_ = user;
    digest_password_ = password;
    digest_nonce_ = "5f0b1e2c3d4a5968778899aabbccddee";
}

bool OnvifCameraEmulator::check_digest(struct MHD_Connection* conn,
                                       const char* method,
                                       const char* url) const {
    const char* auth = MHD_lookup_connection_value(conn, MHD_HEADER_KIND,
                                                   "Authorization");
    if (!auth) return false;
    const std::string hdr = auth;

    const std::string user     = digest_param(hdr, "username");
    const std::string realm    = digest_param(hdr, "realm");
    const std::string nonce    = digest_param(hdr, "nonce");
    const std::string uri      = digest_param(hdr, "uri");
    const std::string qop      = digest_param(hdr, "qop");
    const std::string nc       = digest_param(hdr, "nc");
    const std::string cnonce   = digest_param(hdr, "cnonce");
    const std::string response = digest_param(hdr, "response");
    // The client's digest uri is the full request target ("/onvif/x?Idx=5")
    // while MHD's url holds the path alone.
    const std::size_t query = uri.find('?');
    const std::string path = query == std::string::npos ? uri : uri.substr(0, query);
    if (user != digest_user_ || realm != "ONVIF" || nonce != digest_nonce_ ||
        path != url || response.empty())
        return false;

    const std::string ha1 = md5_hex(user + ":" + realm + ":" + digest_password_);
    const std::string ha2 = md5_hex(std::string(method) + ":" + uri);
    const std::string expected =
        qop.empty() ? md5_hex(ha1 + ":" + nonce + ":" + ha2)
                    : md5_hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce +
                              ":" + qop + ":" + ha2);
    return expected == response;
}

MHD_Result OnvifCameraEmulator::on_request(
    void* cls, struct MHD_Connection* conn,
    const char* url, const char* method, const char* /*version*/,
    const char* upload_data, size_t* upload_data_size,
    void** con_cls) {
    auto* emulator = static_cast<OnvifCameraEmulator*>(cls);

    // First call for this connection: allocate accumulation buffer
    if (*con_cls == nullptr) {
        *con_cls = new ConnData{};
        return MHD_YES;
    }

    auto* data = static_cast<ConnData*>(*con_cls);

    // Accumulate POST body
    if (*upload_data_size > 0) {
        data->body.append(upload_data, *upload_data_size);
        *upload_data_size = 0;
        return MHD_YES;
    }

    // Body complete — extract SOAPAction
    std::string soap_action;
    const char* sa = MHD_lookup_connection_value(
        conn, MHD_HEADER_KIND, "SOAPAction");
    if (sa) {
        soap_action = sa;
        // Strip surrounding double-quotes if present: "http://..." -> http://...
        if (soap_action.size() >= 2 &&
            soap_action.front() == '"' && soap_action.back() == '"')
            soap_action = soap_action.substr(1, soap_action.size() - 2);
    } else {
        // Fall back to action parameter in Content-Type
        const char* ct = MHD_lookup_connection_value(
            conn, MHD_HEADER_KIND, "Content-Type");
        if (ct) {
            std::string cts = ct;
            auto p = cts.find("action=\"");
            if (p != std::string::npos) {
                p += 8;
                auto e = cts.find('"', p);
                if (e != std::string::npos)
                    soap_action = cts.substr(p, e - p);
            }
        }
    }

    // Digest gate: refuse anything the client has not authenticated at the
    // HTTP layer yet, as cameras with ONVIF authentication set to digest do.
    if (emulator->digest_required_ &&
        !emulator->check_digest(conn, method, url)) {
        const std::string challenge =
            "Digest realm=\"ONVIF\", qop=\"auth\", nonce=\"" +
            emulator->digest_nonce_ + "\"";
        const std::string body401 =
            "<html><body><h2>Access Error: 401 -- Unauthorized</h2></body></html>";
        struct MHD_Response* unauthorized = MHD_create_response_from_buffer(
            body401.size(), const_cast<char*>(body401.data()),
            MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(unauthorized, "WWW-Authenticate",
                                challenge.c_str());
        MHD_add_response_header(unauthorized, MHD_HTTP_HEADER_CONTENT_TYPE,
                                "text/html");
        MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_UNAUTHORIZED,
                                           unauthorized);
        MHD_destroy_response(unauthorized);
        return ret;
    }

    // Dispatch
    current_connection() = conn;
    auto [status, body] = emulator->handle(url, soap_action, data->body);
    current_connection() = nullptr;

    struct MHD_Response* response = MHD_create_response_from_buffer(
        body.size(),
        const_cast<char*>(body.data()),
        MHD_RESPMEM_MUST_COPY);

    MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE,
                            "application/soap+xml; charset=utf-8");

    MHD_Result ret = MHD_queue_response(conn, static_cast<unsigned>(status), response);
    MHD_destroy_response(response);
    return ret;
}

void OnvifCameraEmulator::on_completed(
    void* /*cls*/, struct MHD_Connection* /*conn*/,
    void** con_cls, enum MHD_RequestTerminationCode /*toe*/) {
    delete static_cast<ConnData*>(*con_cls);
    *con_cls = nullptr;
}
