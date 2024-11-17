#ifndef WEBSOCKET_ENDPOINT_MANAGER_H
#define WEBSOCKET_ENDPOINT_MANAGER_H

#include <boost/asio.hpp>
#include <csp/adapters/websocket/WebsocketClientTypes.h>
#include <csp/adapters/websocket/WebsocketEndpoint.h>
#include <csp/adapters/websocket/ClientAdapterManager.h>
#include <csp/adapters/websocket/ClientInputAdapter.h>
#include <csp/adapters/websocket/ClientOutputAdapter.h>
#include <csp/adapters/websocket/ClientHeaderUpdateAdapter.h>
#include <csp/adapters/websocket/ClientConnectionRequestAdapter.h>
#include <csp/core/Enum.h>
#include <csp/core/Hash.h>
#include <csp/engine/AdapterManager.h>
#include <csp/engine/Dictionary.h>
#include <csp/engine/PushInputAdapter.h>
#include <csp/core/Platform.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <vector>
#include <unordered_set>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <optional>
#include <functional>

namespace csp::adapters::websocket {
using namespace csp;
class WebsocketEndpoint; // Forward declaration

class ClientAdapterManager; //Forward declare
class ClientOutputAdapter;
class ClientConnectionRequestAdapter;


// struct WebsocketClientStatusTypeTraits
// {
//     enum _enum : unsigned char
//     {
//         ACTIVE = 0,
//         GENERIC_ERROR = 1,
//         CONNECTION_FAILED = 2,
//         CLOSED = 3,
//         MESSAGE_SEND_FAIL = 4,

//         NUM_TYPES
//     };
// };
// using ClientStatusType = Enum<WebsocketClientStatusTypeTraits>;

struct ConnectPayloads {
    std::vector<std::string> consumer_payloads;
    std::vector<std::string> producer_payloads;
};

struct EndpointConfig {
    std::chrono::milliseconds reconnect_interval;
    std::unique_ptr<boost::asio::steady_timer> reconnect_timer;
    bool attempting_reconnect{false};
    bool shutting_down{false};
    
    // Payloads for different client types
    std::vector<std::string> consumer_payloads;
    std::vector<std::string> producer_payloads;

    explicit EndpointConfig(boost::asio::io_context& ioc) 
        : reconnect_timer(std::make_unique<boost::asio::steady_timer>(ioc)) {}
};

// Callbacks for endpoint events
struct EndpointCallbacks {
    std::function<void(const std::string&)> onOpen;
    std::function<void(const std::string&, const std::string&)> onFail;
    std::function<void(const std::string&)> onClose;
    std::function<void(const std::string&, const std::string&)> onSendFail;
    std::function<void(const std::string&, void*, size_t)> onMessage;
};

class WebsocketEndpointManager {
public:
    explicit WebsocketEndpointManager(ClientAdapterManager* mgr, const Dictionary & properties, Engine* engine);

    void send(const std::string& value, const size_t& caller_id);
    bool adapterPruned( size_t caller_id );
    void start(DateTime starttime, DateTime endtime);
    void stop();
    void handleConnectionRequest( const Dictionary & properties);    // void removeEndpoint(const std::string& id);

    void handleEndpointFailure(const std::string& endpoint_id, const std::string& reason, ClientStatusType status_type);
    void handleEndpointClosure(const std::string& endpoint_id);
    void setupEndpoint(const std::string& endpoint_id, std::unique_ptr<WebsocketEndpoint> endpoint, std::string payload, bool persist, bool is_consumer, size_t validated_id);
    void shutdownEndpoint(const std::string& endpoint_id);

    void removeEndpointForCallerId(const std::string& endpoint_id, bool is_consumer, size_t validated_id);

    void ensureVectorSize(std::vector<bool>& vec, size_t caller_id);

    void addConsumer(const std::string& endpoint_id, size_t caller_id);

    void addProducer(const std::string& endpoint_id, size_t caller_id);

    bool canRemoveEndpoint(const std::string& endpoint_id);

    void removeConsumer(const std::string& endpoint_id, size_t caller_id);

    void removeProducer(const std::string& endpoint_id, size_t caller_id);


    PushInputAdapter * getInputAdapter( CspTypePtr & type, PushMode pushMode, const Dictionary & properties );
    OutputAdapter * getOutputAdapter( const Dictionary & properties );
    OutputAdapter * getHeaderUpdateAdapter();
    OutputAdapter * getConnectionRequestAdapter( const Dictionary & properties );
private:
    inline size_t validateCallerId(int64_t caller_id) const {
        if (caller_id < 0) {
            CSP_THROW(ValueError, "caller_id cannot be negative: " << caller_id);
        }
        return static_cast<size_t>(caller_id);
    }
    using UriInfo = std::tuple<int32_t, std::unordered_set<uint64_t>>; //TODO remove
    using OptWorkGuard = std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>;
    std::unique_ptr<WebsocketEndpointManager> m_endpointManager;
    net::io_context m_ioc;
    Engine* m_engine;
    ClientAdapterManager* m_mgr;
    [[maybe_unused]] bool m_active;
    [[maybe_unused]] std::atomic<bool> m_shouldRun{false};
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::unique_ptr<WebsocketEndpoint> m_endpoint;  // TODO remove
    [[maybe_unused]] ClientInputAdapter* m_inputAdapter;
    [[maybe_unused]] ClientOutputAdapter* m_outputAdapter;
    ClientHeaderUpdateOutputAdapter* m_updateAdapter;
    std::unique_ptr<std::thread> m_thread;
    Dictionary m_properties;
    // For each subscribe call, which uri's it is subscribed to
    std::vector<std::unordered_set<std::string>> m_subscribeFromUri;
    // For each send call, which uri's it will send out to
    std::vector<std::unordered_set<std::string>> m_sendToUri;

    // uri -> (send_calls, set of caller id's for the subscribtions)
    // If send_calls is 0 (no adapter is sending out to that uri)
    // AND the subscriptions set is empty, we can then shutdown the encpoint.
    std::unordered_map<std::string, UriInfo> m_uriInfo; //TODO: remove
    //unclear if this is needed to be on the object
    std::vector<ClientConnectionRequestAdapter*> m_connectionRequestAdapters;

    // Bidirectional mapping using vectors since caller_ids are sequential
    // Maybe not efficient? Should be good for small number of edges though
    std::unordered_map<std::string, std::vector<bool>> m_endpoint_consumers;  // endpoint_id -> vector[caller_id] for consuemrs
    std::unordered_map<std::string, std::vector<bool>> m_endpoint_producers;  // endpoint_id -> vector[caller_id] for producers
    
    // Quick lookup for caller's endpoints
    std::vector< std::unordered_set<std::string> > m_consumer_endpoints;  // caller_id -> set of endpoints they consume from
    std::vector< std::unordered_set<std::string> > m_producer_endpoints;  // caller_id -> set of endpoints they produce to
    OptWorkGuard m_work_guard;
    std::unordered_map<std::string, std::unique_ptr<WebsocketEndpoint>> m_endpoints;
    std::unordered_map<std::string, ConnectPayloads> m_connect_payloads;
    std::unordered_map<std::string, EndpointConfig> m_endpoint_configs;
    std::vector<ClientInputAdapter*> m_inputAdapters;
    std::vector<ClientOutputAdapter*> m_outputAdapters;
    bool m_dynamic;
};

}
#endif