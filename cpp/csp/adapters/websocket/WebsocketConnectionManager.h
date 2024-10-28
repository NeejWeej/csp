// // WebsocketConnectionManager.h
// #ifndef _IN_CSP_ADAPTERS_WEBSOCKETS_CLIENT_ADAPTERMGR_H
// #define _IN_CSP_ADAPTERS_WEBSOCKETS_CLIENT_ADAPTERMGR_H

// #include <unordered_map>
// #include <mutex>
// #include <memory>
// #include <thread>
// #include <csp/adapters/websocket/csp_autogen/websocket_types.h>
// #include <csp/adapters/websocket/WebsocketEndpoint.h>

// namespace csp::adapters::websocket {
// using namespace csp;

// class WebsocketConnectionManager {
// public:
//     WebsocketConnectionManager(boost::asio::io_context& ioc);
    
//     std::shared_ptr<WebsocketEndpoint> createConnection(const autogen::ConnectionRequest& request);
//     void removeConnection(const std::string& url);
//     std::shared_ptr<WebsocketEndpoint> getConnection(const std::string& url);
    
//     void run();
//     void stop();

// private:
//     boost::asio::io_context& m_ioc;
//     std::unordered_map<std::string, std::shared_ptr<WebsocketEndpoint>> m_connections;
//     std::mutex m_mutex;
//     std::unique_ptr<std::thread> m_thread;
//     bool m_shouldRun;
// };
// }
// #endif