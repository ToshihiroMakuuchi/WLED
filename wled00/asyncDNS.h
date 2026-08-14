#pragma once
/*
  asyncDNS.h - wrapper class for asynchronous DNS lookups using lwIP
  by @dedehai, C++ improvements & hardening by @willmmiles

  M5Stack CoreS3 / WLED V17 compatibility patch
  Phase 10.4.2P-V17i-DNS

  ESP-IDF 5.x change:
  -------------------
  dns_gethostbyname() is a lwIP raw API. On ESP-IDF 5.x, calling it
  directly from the Arduino/WLED loop task can trigger:

    assert failed: udp_new_ip_type ...
    (Required to lock TCPIP core functionality!)

  For ESP32 + ESP-IDF >= 5, this patched implementation schedules the
  actual dns_gethostbyname() call into the lwIP TCP/IP task by using
  tcpip_callback().

  ESP8266 and ESP32/ESP-IDF 4.x retain WLED's original behavior.
*/

#include <Arduino.h>
#include <atomic>
#include <memory>
#include <new>
#include <cstring>
#include <cstdlib>

#include <lwip/dns.h>
#include <lwip/err.h>

#if defined(ARDUINO_ARCH_ESP32)
  #include <esp_idf_version.h>

  #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    #include <lwip/tcpip.h>
    #define WLED_ASYNCDNS_TCPIP_SAFE 1
  #else
    #define WLED_ASYNCDNS_TCPIP_SAFE 0
  #endif
#else
  #define WLED_ASYNCDNS_TCPIP_SAFE 0
#endif


class AsyncDNS {

  // C++14 shim
#if __cplusplus < 201402L
  template<class T, class... Args>
  static std::unique_ptr<T>
  make_unique(Args&&... args)
  {
      return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
  }
#endif

public:
  // note: passing the IP as a pointer to query() is not implemented because
  // it is not thread-safe without mutexes.
  enum class result { Idle, Busy, Success, Error };

  // -------------------------------------------------------------------------
  // Non-blocking query function
  // -------------------------------------------------------------------------
  static std::shared_ptr<AsyncDNS> query(
      const char* hostname,
      std::shared_ptr<AsyncDNS> current = {})
  {
    if (!hostname || !hostname[0]) {
      return {};
    }

    if (!current || (current->_status == result::Busy)) {
      current.reset(new AsyncDNS());
    }

    current->_status = result::Busy;

#if WLED_ASYNCDNS_TCPIP_SAFE

    // ESP-IDF 5.x:
    // Run the lwIP raw DNS API from the TCP/IP task context.
    QueryContext* context =
        new (std::nothrow) QueryContext(current, hostname);

    if (!context || !context->hostname) {
      delete context;
      current->_status = result::Error;
      current->_errorcount++;
      return current;
    }

    const err_t postResult =
        tcpip_callback(_dns_start_tcpip, context);

    if (postResult != ERR_OK) {
      delete context;
      current->_status = result::Error;
      current->_errorcount++;
    }

    return current;

#else

    // Original WLED behavior for ESP8266 and ESP-IDF 4.x.
#if __cplusplus >= 201402L
    using std::make_unique;
#endif

    std::unique_ptr<std::shared_ptr<AsyncDNS>> callback_state =
        make_unique<std::shared_ptr<AsyncDNS>>(current);

    if (!callback_state) {
      current->_status = result::Error;
      current->_errorcount++;
      return current;
    }

    const err_t err =
        dns_gethostbyname(
          hostname,
          &current->_raw_addr,
          _dns_callback_legacy,
          callback_state.get()
        );

    if (err == ERR_OK) {
      current->_status = result::Success; // result already in cache
    }
    else if (err == ERR_INPROGRESS) {
      callback_state.release(); // callback owns it now
    }
    else {
      current->_status = result::Error;
      current->_errorcount++;
    }

    return current;

#endif
  }


  // -------------------------------------------------------------------------
  // Get IP once Success is returned
  // -------------------------------------------------------------------------
  const IPAddress getIP()
  {
    if (_status != result::Success) {
      return IPAddress(0, 0, 0, 0);
    }

#ifdef ARDUINO_ARCH_ESP32
    return IPAddress(_raw_addr.u_addr.ip4.addr);
#else
    return IPAddress(_raw_addr.addr);
#endif
  }


  void reset()
  {
    _errorcount = 0;
  }


  const result status()
  {
    return _status;
  }


  const uint16_t getErrorCount()
  {
    return _errorcount;
  }


private:
  ip_addr_t _raw_addr {};
  std::atomic<result> _status { result::Idle };
  uint16_t _errorcount = 0;

  AsyncDNS() {}


#if WLED_ASYNCDNS_TCPIP_SAFE

  // -------------------------------------------------------------------------
  // ESP-IDF 5.x TCP/IP task context
  // -------------------------------------------------------------------------

  struct QueryContext {
    std::shared_ptr<AsyncDNS> instance;
    char* hostname;

    QueryContext(
        const std::shared_ptr<AsyncDNS>& dnsInstance,
        const char* name)
      : instance(dnsInstance),
        hostname(name ? strdup(name) : nullptr)
    {
    }

    ~QueryContext()
    {
      if (hostname) {
        free(hostname);
        hostname = nullptr;
      }
    }
  };


  // Runs inside the lwIP TCP/IP task.
  static void _dns_start_tcpip(void* arg)
  {
    QueryContext* context =
        reinterpret_cast<QueryContext*>(arg);

    if (!context ||
        !context->instance ||
        !context->hostname) {

      delete context;
      return;
    }

    AsyncDNS& instance = *context->instance;

    const err_t err =
        dns_gethostbyname(
          context->hostname,
          &instance._raw_addr,
          _dns_callback_tcpip,
          context
        );

    if (err == ERR_OK) {
      // Address was already cached; callback will not run.
      instance._status = result::Success;
      delete context;
    }
    else if (err == ERR_INPROGRESS) {
      // DNS subsystem owns the callback path now.
      // QueryContext is deleted by _dns_callback_tcpip().
    }
    else {
      instance._status = result::Error;
      instance._errorcount++;
      delete context;
    }
  }


  // Called by lwIP when the asynchronous DNS lookup completes.
  // This callback also runs in TCP/IP task context.
  static void _dns_callback_tcpip(
      const char* name,
      const ip_addr_t* ipaddr,
      void* arg)
  {
    (void)name;

    QueryContext* context =
        reinterpret_cast<QueryContext*>(arg);

    if (!context || !context->instance) {
      delete context;
      return;
    }

    AsyncDNS& instance = *context->instance;

    if (ipaddr) {
      instance._raw_addr = *ipaddr;
      instance._status = result::Success;
    }
    else {
      instance._status = result::Error;
      instance._errorcount++;
    }

    delete context;
  }

#endif // WLED_ASYNCDNS_TCPIP_SAFE


  // -------------------------------------------------------------------------
  // Original callback path for ESP8266 / ESP-IDF 4.x
  // -------------------------------------------------------------------------

  static void _dns_callback_legacy(
      const char* name,
      const ip_addr_t* ipaddr,
      void* arg)
  {
    (void)name;

    std::shared_ptr<AsyncDNS>* instance_ptr =
        reinterpret_cast<std::shared_ptr<AsyncDNS>*>(arg);

    if (!instance_ptr || !(*instance_ptr)) {
      delete instance_ptr;
      return;
    }

    AsyncDNS& instance = **instance_ptr;

    if (ipaddr) {
      instance._raw_addr = *ipaddr;
      instance._status = result::Success;
    }
    else {
      instance._status = result::Error;
      instance._errorcount++;
    }

    delete instance_ptr;
  }
};

#undef WLED_ASYNCDNS_TCPIP_SAFE
