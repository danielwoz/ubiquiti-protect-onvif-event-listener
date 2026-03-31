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

#pragma once

#include <microhttpd.h>
// MHD_Result was introduced in 0.9.71; older sysroots use plain int.
#if MHD_VERSION < 0x00097100
typedef int MHD_Result;
#endif
#include <cstdint>
#include <string>
#include <utility>

/**
 * OnvifCameraEmulator
 *
 * Base class for ONVIF camera emulators used in testing.
 * Runs a minimal HTTP server (via libmicrohttpd) that accepts SOAP POST
 * requests and delegates each one to the subclass's handle() method.
 *
 * Typical usage:
 *   class MyCamEmulator : public OnvifCameraEmulator {
 *   protected:
 *       std::pair<int,std::string> handle(path, soap_action, body) override;
 *   };
 *
 *   MyCamEmulator emu("192.168.1.108");
 *   emu.start();
 *   // pass emu.local_address() as CameraConfig::ip to OnvifListener
 *   emu.stop(); // or just let the destructor do it
 */
class OnvifCameraEmulator {
 public:
    /// @param real_ip  The real camera IP whose URLs appear in recorded
    ///                 response bodies (used by rewrite_urls()).
    explicit OnvifCameraEmulator(const std::string& real_ip);
    virtual ~OnvifCameraEmulator();

    OnvifCameraEmulator(const OnvifCameraEmulator&)            = delete;
    OnvifCameraEmulator& operator=(const OnvifCameraEmulator&) = delete;

    /// Start the HTTP server on a random available port.
    void start();
    void stop();

    uint16_t port() const { return port_; }

    /// Returns "127.0.0.1:<port>" — use as CameraConfig::ip in tests.
    std::string local_address() const;

    /// Set the alarm service base URL to advertise in GetServices responses
    /// (e.g. the UosEmulator's base_url() in tests).  Empty by default,
    /// which means no alarm service entry is included in GetServices.
    void set_alarm_service_url(const std::string& url) { alarm_service_url_ = url; }

 protected:
    /// Called for each complete SOAP request. Return {http_status, soap_xml}.
    /// @param path        Request URL path  (e.g. "/onvif/event_service")
    /// @param soap_action SOAP action URI, quotes stripped
    /// @param body        Raw request body (complete SOAP XML envelope)
    virtual std::pair<int, std::string> handle(
        const std::string& path,
        const std::string& soap_action,
        const std::string& body) = 0;

    /// Rewrite occurrences of the real camera IP in a response body so
    /// that the listener follows URLs back to this local emulator.
    std::string rewrite_urls(const std::string& response) const;

    const std::string real_ip_;
    std::string       alarm_service_url_;

 private:
    // Per-connection accumulation buffer (stored in MHD con_cls)
    struct ConnData { std::string body; };

    static MHD_Result on_request(
        void* cls, struct MHD_Connection* conn,
        const char* url, const char* method, const char* version,
        const char* upload_data, size_t* upload_data_size,
        void** con_cls);

    static void on_completed(
        void* cls, struct MHD_Connection* conn,
        void** con_cls, enum MHD_RequestTerminationCode toe);

    struct MHD_Daemon* daemon_{nullptr};
    uint16_t           port_{0};
};
