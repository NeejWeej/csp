// // WebsocketConnectionManager.cpp

// #include "WebsocketConnectionManager.h"
// #include "WebsocketEndpoint.h"
// #include <csp/core/Exception.h>
// #include <boost/asio/io_context.hpp>
// #include <boost/beast/core.hpp>
// #include <boost/beast/websocket.hpp>

// namespace csp::adapters::websocket {

// namespace beast = boost::beast;
// namespace websocket = beast::websocket;
// namespace net = boost::asio;
// using tcp = boost::asio::ip::tcp;

// // Consider using this class to manage connections
// // It should add new connections and route to the corresponding
// // edges in csp. NOTE!! We should return a tuple/csp.struct of (uri, value)
// // pairs when subscribing

// WebsocketConnectionManager::WebsocketConnectionManager(net::io_context& ioc)
//     : m_ioc(ioc) {
// }

// std::shared_ptr<WebsocketEndpoint> WebsocketConnectionManager::createConnection(const autogen::ConnectionRequest& request) {
//     std::lock_guard<std::mutex> lock(m_mutex);
    
//     auto it = m_connections.find(request.url());
//     if (it != m_connections.end()) {
//         return it->second;
//     }

//     try {
//         auto endpoint = std::make_shared<WebsocketEndpoint>(request);
//         m_connections[request.url()] = endpoint;
//         return endpoint;
//     } catch (const std::exception& e) {
//         CSP_THROW(RuntimeException, "Failed to create connection to " << request.url() << ": " << e.what());
//     }
// }

// void WebsocketConnectionManager::removeConnection(const std::string& url) {
//     std::lock_guard<std::mutex> lock(m_mutex);
//     auto it = m_connections.find(url);
//     if (it != m_connections.end()) {
//         m_connections.erase(it);
//     }
// }

// std::shared_ptr<WebsocketEndpoint> WebsocketConnectionManager::getConnection(const std::string& url) {
//     std::lock_guard<std::mutex> lock(m_mutex);
//     auto it = m_connections.find(url);
//     return (it != m_connections.end()) ? it->second : nullptr;
// }

// void WebsocketConnectionManager::run() {
//     if (m_thread) {
//         CSP_THROW(RuntimeException, "WebsocketConnectionManager is already running");
//     }

//     m_thread = std::make_unique<std::thread>([this]() {
//         while( m_shouldRun )
//         {
//             // m_endpoint -> run();
//             // How do we add more endpoints here?
//             // m_active = false;
//             if( m_shouldRun ) sleep( m_properties.get<TimeDelta>( "reconnect_interval" ) );
//         }
//     });
// }

// void WebsocketConnectionManager::stop() {
//     m_ioc.stop();
    
//     if (m_thread && m_thread->joinable()) {
//         m_thread->join();
//     }
//     m_thread.reset();

//     std::lock_guard<std::mutex> lock(m_mutex);
//     m_connections.clear();
// }

// } // namespace csp::adapters::websocket