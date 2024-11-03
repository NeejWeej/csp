#include <csp/adapters/websocket/ClientAdapterManager.h>
#include <boost/system/error_code.hpp>
#include <iostream>

namespace csp {

INIT_CSP_ENUM( adapters::websocket::ClientStatusType,
               "ACTIVE",
               "GENERIC_ERROR",
               "CONNECTION_FAILED",
               "CLOSED",
               "MESSAGE_SEND_FAIL",
);

}

// With TLS
namespace csp::adapters::websocket {

ClientAdapterManager::ClientAdapterManager( Engine* engine, const Dictionary & properties ) 
: AdapterManager( engine ), 
    m_ioc(),
    m_active( false ), 
    m_shouldRun( false ), 
    m_endpoint( std::make_unique<WebsocketEndpoint>( m_ioc, properties ) ),
    m_inputAdapter( nullptr ), 
    m_outputAdapter( nullptr ),
    m_updateAdapter( nullptr ),
    m_thread( nullptr ), 
    m_properties( properties ),
    m_work_guard(boost::asio::make_work_guard(m_ioc)),
    m_dynamic( properties.get<bool>("dynamic") )
{ };

ClientAdapterManager::~ClientAdapterManager()
{ };

void ClientAdapterManager::start(DateTime starttime, DateTime endtime) {
    AdapterManager::start(starttime, endtime);
    if( m_dynamic ){
        m_thread = std::make_unique<std::thread>([this]() {
            m_ioc.run();
        });
    }
    else {
        m_shouldRun = true;
        m_endpoint -> setOnOpen(
            [ this ]() {
                m_active = true;
                pushStatus( StatusLevel::INFO, ClientStatusType::ACTIVE, "Connected successfully" );
            }
        );
        m_endpoint -> setOnFail(
            [ this ]( const std::string& reason ) {
                std::stringstream ss;
                ss << "Connection Failure: " << reason;
                m_active = false;
                pushStatus( StatusLevel::ERROR, ClientStatusType::CONNECTION_FAILED, ss.str() );
            } 
        );
        if( m_inputAdapter ) {
            m_endpoint -> setOnMessage(
                [ this ]( void* c, size_t t ) {
                    std::cout << "YUR";
                    PushBatch batch( m_engine -> rootEngine() );
                    m_inputAdapter -> processMessage( c, t, &batch );
                }
            );
        } else {
            // if a user doesn't call WebsocketAdapterManager.subscribe, no inputadapter will be created
            // but we still need something to avoid on_message_cb not being set in the endpoint.
            m_endpoint -> setOnMessage( []( void* c, size_t t ){} );
        }
        m_endpoint -> setOnClose(
            [ this ]() {
                m_active = false;
                pushStatus( StatusLevel::INFO, ClientStatusType::CLOSED, "Connection closed" );
            }
        );
        m_endpoint -> setOnSendFail(
            [ this ]( const std::string& s ) {
                std::stringstream ss;
                ss << "Failed to send: " << s;
                pushStatus( StatusLevel::ERROR, ClientStatusType::MESSAGE_SEND_FAIL, ss.str() );
            }
        );

        m_thread = std::make_unique<std::thread>( [ this ]() { 
            while( m_shouldRun )
            {
                std::cout << "WE ARE RUNNING\n"; 
                m_endpoint -> run();
                std::cout << "WE ARE NOT RUNNING\n"; 
                m_active = false;
                if( m_shouldRun ) sleep( m_properties.get<TimeDelta>( "reconnect_interval" ) );
            }
        });
    }
};

void ClientAdapterManager::send(const std::string& value, const size_t& caller_id) {
    // Safety check for caller_id
    if (caller_id >= m_producer_endpoints.size()) {
        return;
    }

    // Get all endpoints this producer is connected to
    const auto& endpoints = m_producer_endpoints[caller_id];
    
    // For each endpoint this producer is connected to
    for (const auto& endpoint_id : endpoints) {
        // Double check the endpoint exists and producer is still valid
        if (auto it = m_endpoints.find(endpoint_id); 
            it != m_endpoints.end() && 
            caller_id < m_endpoint_producers[endpoint_id].size() && 
            m_endpoint_producers[endpoint_id][caller_id]) {
            
            boost::asio::post(m_ioc, [endpoint_id, value, ep = it->second.get()]() {
                ep->send(value);
            });
        }
    }
}

void ClientAdapterManager::setupOneOffConnection(const std::string& endpoint_id, const Dictionary& properties) {
    auto caller_id = properties.get<int64_t>("caller_id");
    size_t validated_id = validateCallerId(caller_id);
    auto is_consumer = properties.get<bool>("is_subscribe");
    auto payload = properties.get<std::string>("on_connect_payload");
    
    // First add them as consumer/producer so the endpoint stays alive 
    if (is_consumer) {
        addConsumer(endpoint_id, validated_id);
    } else {
        addProducer(endpoint_id, validated_id);
    }

    // If endpoint doesn't exist, create it
    if (!m_endpoints.contains(endpoint_id)) {
        auto endpoint = std::make_unique<WebsocketEndpoint>(m_ioc, properties);
        
        boost::asio::post(m_ioc, [this, endpoint_id, payload, validated_id, is_consumer, 
                          ep = std::move(endpoint)]() mutable {
            // Special onOpen for one-off: send payload and disconnect
            ep->setOnOpen([this, endpoint_id, payload, validated_id, is_consumer]() {
                auto* endpoint = m_endpoints[endpoint_id].get();
                
                // Send the payload
                if (!payload.empty()) {
                    endpoint->send(payload);
                }
                
                // Remove them and check if we should close endpoint
                if (is_consumer) {
                    removeConsumer(endpoint_id, validated_id);
                } else {
                    removeProducer(endpoint_id, validated_id);
                }
                
                if (canRemoveEndpoint(endpoint_id)) {
                    shutdownEndpoint(endpoint_id);
                }
            });
            
            // Still need failure handling
            ep->setOnFail([this, endpoint_id](const std::string& reason) {
                handleEndpointFailure(endpoint_id, reason, ClientStatusType::CONNECTION_FAILED);
            });
            ep -> setOnMessage( []( void* c, size_t t ){} );
            ep -> setOnClose(
                [ this ]() {
                    m_active = false;
                    pushStatus( StatusLevel::INFO, ClientStatusType::CLOSED, "Connection closed" );
                }
            );
            ep -> setOnSendFail(
                [ this ]( const std::string& s ) {
                    std::stringstream ss;
                    ss << "Failed to send: " << s;
                    pushStatus( StatusLevel::ERROR, ClientStatusType::MESSAGE_SEND_FAIL, ss.str() );
                }
            );
            ep -> run();
            m_endpoints[endpoint_id] = std::move(ep);
        });
    } else {
        // Endpoint exists, just send payload and disconnect
        auto* endpoint = m_endpoints[endpoint_id].get();
        if (!payload.empty()) {
            boost::asio::post(m_ioc, [endpoint, payload, this, endpoint_id, 
                              validated_id, is_consumer]() {
                endpoint->send(payload);
                
                // Remove them and maybe close endpoint
                if (is_consumer) {
                    removeConsumer(endpoint_id, validated_id);
                } else {
                    removeProducer(endpoint_id, validated_id);
                }
                
                if (canRemoveEndpoint(endpoint_id)) {
                    shutdownEndpoint(endpoint_id);
                }
            });
        }
    }
}

void ClientAdapterManager::shutdownEndpoint(const std::string& endpoint_id) {
    boost::asio::post(m_ioc, [this, endpoint_id]() {
        // Cancel any pending reconnection attempts
        if (auto config_it = m_endpoint_configs.find(endpoint_id); 
            config_it != m_endpoint_configs.end()) {
            config_it->second.reconnect_timer->cancel();
            // This won't have us revive on the onFail callback
            config_it->second.shutting_down = true;
            m_endpoint_configs.erase(config_it);
        }
        
        // Stop and remove the endpoint
        if (auto endpoint_it = m_endpoints.find(endpoint_id); 
            endpoint_it != m_endpoints.end()) {
            endpoint_it->second->stop();
            m_endpoints.erase(endpoint_it);
        }
    });
}

void ClientAdapterManager::setupEndpoint(const std::string& endpoint_id, 
                                       std::unique_ptr<WebsocketEndpoint>& endpoint) {
    boost::asio::post(m_ioc, [this, endpoint_id, ep = std::move(endpoint)]() mutable {
        ep->setOnOpen([this, endpoint_id]() {
            auto [iter, inserted] = m_endpoint_configs.try_emplace(endpoint_id, m_ioc);
            auto& config = iter->second;
            config.attempting_reconnect = false;
            
            // Send all stored payloads for active consumers and producers
            auto* endpoint = m_endpoints[endpoint_id].get();
            
            // Send consumer payloads
            const auto& consumers = m_endpoint_consumers[endpoint_id];
            for (size_t i = 0; i < config.consumer_payloads.size(); ++i) {
                if (!config.consumer_payloads[i].empty() && 
                    i < consumers.size() && consumers[i]) {
                    endpoint->send(config.consumer_payloads[i]);
                }
            }
            
            // Send producer payloads
            const auto& producers = m_endpoint_producers[endpoint_id];
            for (size_t i = 0; i < config.producer_payloads.size(); ++i) {
                if (!config.producer_payloads[i].empty() && 
                    i < producers.size() && producers[i]) {
                    endpoint->send(config.producer_payloads[i]);
                }
            }
            
            pushStatus(StatusLevel::INFO, ClientStatusType::ACTIVE, 
                      "Connected successfully for endpoint " + endpoint_id);
        });
        
        ep->setOnFail([this, endpoint_id](const std::string& reason) {
            handleEndpointFailure(endpoint_id, reason, ClientStatusType::CONNECTION_FAILED);
        });
        
        ep->setOnClose([this, endpoint_id]() {
            handleEndpointClosure(endpoint_id);
        });
        ep->setOnMessage([this, endpoint_id](void* data, size_t len) {
            // Here we need to route to all active consumers for this endpoint
            const auto& consumers = m_endpoint_consumers[endpoint_id];
            
            // For each active consumer, we need to send to their input adapter
            PushBatch batch( m_engine -> rootEngine() );  // TODO is this right?
            for (size_t consumer_id = 0; consumer_id < consumers.size(); ++consumer_id) {
                if (consumers[consumer_id]) {
                    auto tup = std::tuple<std::string, void*> {endpoint_id, data};
                    m_inputAdapters[consumer_id] -> processMessage( tup, len, &batch );
                }
            }
        });
        ep -> run();
        m_endpoints[endpoint_id] = std::move(ep);
    });
}


void ClientAdapterManager::handleEndpointFailure(const std::string& endpoint_id, 
                                               const std::string& reason, ClientStatusType status_type) {
    // If there are any active consumers/producers, try to reconnect
    if (!canRemoveEndpoint(endpoint_id)) {
        auto [iter, inserted] = m_endpoint_configs.try_emplace(endpoint_id, m_ioc);
        auto& config = iter->second;
        
        if (!config.attempting_reconnect) {
            config.attempting_reconnect = true;
            
            // Schedule reconnection attempt
            config.reconnect_timer->expires_after(config.reconnect_interval);
            config.reconnect_timer->async_wait([this, endpoint_id](const boost::system::error_code& ec) {
                boost::asio::post(m_ioc, [this, endpoint_id]() {
                    if (auto it = m_endpoints.find(endpoint_id); 
                        it != m_endpoints.end()) {
                        auto config_it = m_endpoint_configs.find(endpoint_id);
                        if (config_it != m_endpoint_configs.end()) {
                            auto& config = config_it -> second;
                            config.attempting_reconnect = false;
                        }
                        it->second->run();  // Attempt to reconnect
                        // Could this have been deleted??
                    }
                });
            });
        }
    } else {
        // No active consumers/producers, clean up the endpoint
        boost::asio::post(m_ioc, [this, endpoint_id]() {
            m_endpoints.erase(endpoint_id);
            m_endpoint_configs.erase(endpoint_id);
        });
    }
    
    std::stringstream ss;
    ss << "Connection Failure for " << endpoint_id << ": " << reason;
    pushStatus(StatusLevel::ERROR, status_type, ss.str());
}

void ClientAdapterManager::handleEndpointClosure(const std::string& endpoint_id) {
    if (auto config_it = m_endpoint_configs.find(endpoint_id); 
        config_it != m_endpoint_configs.end() && !config_it->second.shutting_down) {
        // Only handle as failure if not intentionally shutting down
        handleEndpointFailure(endpoint_id, "Connection closed", ClientStatusType::CLOSED);
    }
}

void ClientAdapterManager::handleConnectionRequest(const Dictionary & properties)
{
    auto endpoint_id = properties.get<std::string>("uri");
    auto caller_id = properties.get<int64_t>("caller_id");
    size_t validated_id = validateCallerId(caller_id);
    autogen::ActionType action = autogen::ActionType::create( properties.get<std::string>("action") );
    auto is_consumer = properties.get<bool>("is_subscribe");
    
    switch(action.enum_value()) {
        case autogen::ActionType::enum_::CONNECT: {
            auto persistent = properties.get<bool>("persistent");
            if (!persistent){
                ClientAdapterManager::setupOneOffConnection(endpoint_id, properties);
            }
            else {
                bool is_new_endpoint = !m_endpoints.contains(endpoint_id);

                auto reconnect_interval = properties.get<TimeDelta>("reconnect_interval");
                // Update endpoint config
                auto& config = m_endpoint_configs.try_emplace(endpoint_id, m_ioc).first->second;

                config.reconnect_interval = std::chrono::milliseconds(
                    reconnect_interval.asMilliseconds()
                );
                // Get payload if it exists
                if (auto payload = properties.get<std::string>("on_connect_payload"); !payload.empty()) {
                    auto& payloads = is_consumer ? config.consumer_payloads : config.producer_payloads;
                    if (payloads.size() <= validated_id) {
                        payloads.resize(validated_id + 1);
                    }
                    payloads[validated_id] = std::move(payload);
                }
                
                if (is_consumer) {
                    ClientAdapterManager::addConsumer(endpoint_id, validated_id);
                } else {
                    ClientAdapterManager::addProducer(endpoint_id, validated_id);
                }
                
                if (is_new_endpoint) {
                    auto endpoint = std::make_unique<WebsocketEndpoint>(m_ioc, properties);
                    ClientAdapterManager::setupEndpoint(endpoint_id, endpoint);
                }
            }
            break;
        }
        
        case csp::autogen::ActionType::enum_::DISCONNECT: {
            // Clear persistence flag for this caller
            if (auto config_it = m_endpoint_configs.find(endpoint_id); config_it != m_endpoint_configs.end()) {
                if (is_consumer) {
                    ClientAdapterManager::removeConsumer(endpoint_id, validated_id);
                } else {
                    ClientAdapterManager::removeProducer(endpoint_id, validated_id);
                }
                if (canRemoveEndpoint(endpoint_id)) {
                    shutdownEndpoint(endpoint_id);
                }
            }
            break;
        }
        
        case csp::autogen::ActionType::enum_::PING: {
            // Only ping if the caller is actually connected to this endpoint
            auto& consumers = m_endpoint_consumers[endpoint_id];
            auto& producers = m_endpoint_producers[endpoint_id];

            if ( ( is_consumer && validated_id < consumers.size() && consumers[validated_id] ) || 
                ( !is_consumer && validated_id < producers.size() && producers[validated_id] ) ) {
                    if (auto it = m_endpoints.find(endpoint_id); it != m_endpoints.end()) {
                        boost::asio::post(m_ioc, [ep = it->second.get()]() {
                            ep->ping();
                        });
                    }
            }
            break;
        }
    }
}


// void ClientAdapterManager::removeEndpoint(const std::string& id) {
//     asio::post(m_ioc, [this, id]() {
//         if (auto it = m_endpoints.find(id); it != m_endpoints.end()) {
//             it->second->stop();
//             m_endpoints.erase(it);
//         }
//     });
// }

void ClientAdapterManager::ensureVectorSize(std::vector<bool>& vec, size_t caller_id) {
    if (vec.size() <= caller_id) {
        vec.resize(caller_id + 1, false);
    }
}

void ClientAdapterManager::ensureCallerVectorsSize(size_t caller_id) {
    if (m_consumer_endpoints.size() <= caller_id) {
        m_consumer_endpoints.resize(caller_id + 1);
    }
    if (m_producer_endpoints.size() <= caller_id) {
        m_producer_endpoints.resize(caller_id + 1);
    }
}

void ClientAdapterManager::addConsumer(const std::string& endpoint_id, size_t caller_id) {
    ensureVectorSize(m_endpoint_consumers[endpoint_id], caller_id);
    m_endpoint_consumers[endpoint_id][caller_id] = true;
    
    ensureCallerVectorsSize(caller_id);
    m_consumer_endpoints[caller_id].insert(endpoint_id);
}

void ClientAdapterManager::addProducer(const std::string& endpoint_id, size_t caller_id) {
    ensureVectorSize(m_endpoint_producers[endpoint_id], caller_id);
    m_endpoint_producers[endpoint_id][caller_id] = true;
    
    ensureCallerVectorsSize(caller_id);
    m_producer_endpoints[caller_id].insert(endpoint_id);
}

bool ClientAdapterManager::canRemoveEndpoint(const std::string& endpoint_id) {
    const auto& consumers = m_endpoint_consumers[endpoint_id];
    const auto& producers = m_endpoint_producers[endpoint_id];
    
    // Check if any true values exist in either vector
    return std::none_of(consumers.begin(), consumers.end(), [](bool b) { return b; }) &&
            std::none_of(producers.begin(), producers.end(), [](bool b) { return b; });
}

void ClientAdapterManager::removeConsumer(const std::string& endpoint_id, size_t caller_id) {
    auto& consumers = m_endpoint_consumers[endpoint_id];
    if (caller_id < consumers.size()) {
        consumers[caller_id] = false;
    }
    
    if (caller_id < m_consumer_endpoints.size()) {
        m_consumer_endpoints[caller_id].erase(endpoint_id);
    }
}

void ClientAdapterManager::removeProducer(const std::string& endpoint_id, size_t caller_id) {
    auto& producers = m_endpoint_producers[endpoint_id];
    if (caller_id < producers.size()) {
        producers[caller_id] = false;
    }
    
    if (caller_id < m_producer_endpoints.size()) {
        m_producer_endpoints[caller_id].erase(endpoint_id);
    }
}


void ClientAdapterManager::stop() {
    AdapterManager::stop();
    if( m_dynamic ){
        // Stop the work guard to allow the io_context to complete
        m_work_guard.reset();
        
        // Stop all endpoints
        for (auto& [endpoint_id, _] : m_endpoints) {
            shutdownEndpoint(endpoint_id);
            // endpoint->stop();
        }
    }
    else{
        m_shouldRun=false; 
        if( m_active ) m_endpoint->stop();
    }
    if( m_thread ) m_thread->join();
};

PushInputAdapter* ClientAdapterManager::getInputAdapter(CspTypePtr & type, PushMode pushMode, const Dictionary & properties)
{   
    if ( m_dynamic ){
        auto input_adapter = m_engine -> createOwnedObject<ClientInputAdapter>(
            // m_engine,
            type,
            pushMode,
            properties    
        );
        m_inputAdapters.push_back(input_adapter);
        return input_adapter;
    }
    if (m_inputAdapter == nullptr)
    {
        m_inputAdapter = m_engine -> createOwnedObject<ClientInputAdapter>(
            // m_engine,
            type,
            pushMode,
            properties    
        );
    }
    return m_inputAdapter;
};

OutputAdapter* ClientAdapterManager::getOutputAdapter( const Dictionary & properties )
{
    if ( m_dynamic ){
        auto caller_id = properties.get<int64_t>("caller_id");
        size_t validated_id = validateCallerId(caller_id);
        assert(!properties.get<bool>("is_subscribe"));
        assert(m_outputAdapters.size() == validated_id);

        auto output_adapter = m_engine -> createOwnedObject<ClientOutputAdapter>( *m_endpoint, this, validated_id, m_ioc, m_dynamic );
        m_outputAdapters.push_back(output_adapter);
        return output_adapter;
    }
    // validated_id does not matter here
    if (m_outputAdapter == nullptr) m_outputAdapter = m_engine -> createOwnedObject<ClientOutputAdapter>( *m_endpoint, this, 0, m_ioc, m_dynamic );
    return m_outputAdapter;
}

OutputAdapter * ClientAdapterManager::getHeaderUpdateAdapter()
{
    if (m_updateAdapter == nullptr) m_updateAdapter = m_engine -> createOwnedObject<ClientHeaderUpdateOutputAdapter>( m_endpoint -> getProperties() );

    return m_updateAdapter;
}

OutputAdapter * ClientAdapterManager::getConnectionRequestAdapter( const Dictionary & properties )
{
    auto caller_id = properties.get<int64_t>("caller_id");
    auto is_subscribe = properties.get<bool>("is_subscribe");
    size_t validated_id = validateCallerId(caller_id); 
    // Select the appropriate vector based on is_subscribe flag
    std::vector<std::unordered_set<std::string>>& target = 
        is_subscribe ? m_subscribeFromUri : m_sendToUri;
    
    // If caller_id is beyond current vector size, add new sets until we reach it
    while (validated_id >= target.size()) {
        target.emplace_back(); 
    }
    
    // Get reference to the hash set for this caller_id
    // std::unordered_set<std::string>& uriSet = target[caller_id];
    
    auto* adapter = m_engine->createOwnedObject<ClientConnectionRequestAdapter>(
        m_endpoint->getProperties(), this, m_ioc
    );
    m_connectionRequestAdapters.push_back(adapter);
    
    return adapter;
}

DateTime ClientAdapterManager::processNextSimTimeSlice( DateTime time )
{
    // no sim data
    return DateTime::NONE();
}

}
