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

MHD_Result OnvifCameraEmulator::on_request(
    void* cls, struct MHD_Connection* conn,
    const char* url, const char* /*method*/, const char* /*version*/,
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
