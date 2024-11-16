#include <csp/adapters/websocket/WebsocketEndpointManager.h>
#include <iostream>

namespace csp::adapters::websocket {

WebsocketEndpointManager::WebsocketEndpointManager( ClientAdapterManager* mgr, const Dictionary & properties, Engine* engine ) 
:   m_ioc(),
    m_engine( engine ),
    m_mgr( mgr ),
    m_active( false ), 
    m_shouldRun( false ), 
    m_endpoint(!properties.get<bool>("dynamic") ? 
        std::make_unique<WebsocketEndpoint>( m_ioc, properties ) : 
        nullptr),
    // m_endpoint( std::make_unique<WebsocketEndpoint>( m_ioc, properties ) ),
    m_inputAdapter( nullptr ), 
    m_outputAdapter( nullptr ),
    m_updateAdapter( nullptr ),
    m_thread( nullptr ), 
    m_properties( properties ),
    m_work_guard(properties.get<bool>("dynamic") ? 
        std::make_optional(boost::asio::make_work_guard(m_ioc)) : 
        std::nullopt),
    m_dynamic( properties.get<bool>("dynamic") )
{
    auto input_size = static_cast<size_t>(properties.get<int64_t>("subscribe_calls"));
    m_inputAdapters.resize(input_size, nullptr);
    m_subscribeFromUri.resize(input_size);
    // send_calls
    auto output_size = static_cast<size_t>(properties.get<int64_t>("send_calls"));
    m_outputAdapters.resize(output_size, nullptr);
    m_sendToUri.resize(output_size);
};

// WebsocketEndpointManager::~WebsocketEndpointManager()
// { };

void WebsocketEndpointManager::start(DateTime starttime, DateTime endtime) {
    // same as dynamic == True
    if( m_endpoint == nullptr ){
        // maybe restart here?
        m_shouldRun = true;
        m_thread = std::make_unique<std::thread>([this]() {
            m_ioc.reset();
            m_ioc.run();
        });
    }
    else {
        // Don't use a work guard here
        // TODO maybe use a work guard here
        // m_work_guard.reset();
        m_shouldRun = true;
        m_endpoint -> setOnOpen(
            [ this ]() {
                m_active = true;
                m_mgr -> pushStatus( StatusLevel::INFO, ClientStatusType::ACTIVE, "Connected successfully" );
            }
        );
        m_endpoint -> setOnFail(
            [ this ]( const std::string& reason ) {
                std::stringstream ss;
                ss << "Connection Failure: " << reason;
                m_active = false;
                m_mgr -> pushStatus( StatusLevel::ERROR, ClientStatusType::CONNECTION_FAILED, ss.str() );
            } 
        );
        if( m_inputAdapter ) {
            m_endpoint -> setOnMessage(
                [ this ]( void* c, size_t t ) {
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
                m_mgr -> pushStatus( StatusLevel::INFO, ClientStatusType::CLOSED, "Connection closed" );
            }
        );
        m_endpoint -> setOnSendFail(
            [ this ]( const std::string& s ) {
                std::stringstream ss;
                ss << "Failed to send: " << s;
                m_mgr -> pushStatus( StatusLevel::ERROR, ClientStatusType::MESSAGE_SEND_FAIL, ss.str() );
            }
        );

        m_thread = std::make_unique<std::thread>( [ this ]() { 
            auto timeout = m_properties.get<TimeDelta>( "reconnect_interval" ).asSeconds();
            while( m_shouldRun )
            {
                m_endpoint -> run();
                m_ioc.run();
                m_active = false;
                m_ioc.reset();
                std::unique_lock<std::mutex> lock(m_mutex);
                if (m_cv.wait_for(lock, 
                    std::chrono::seconds( timeout ), 
                    [this] { return !m_shouldRun; })) {
                    // If condition variable returns true, it means we were signaled to stop
                    break;
                }
            }
        });
    }
};

bool WebsocketEndpointManager::adapterPruned( size_t caller_id ){
    return m_inputAdapters[caller_id] == nullptr;
};

void WebsocketEndpointManager::send(const std::string& value, const size_t& caller_id) {
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
};

void WebsocketEndpointManager::setupOneOffConnection(const std::string& endpoint_id, const Dictionary& properties) {
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
                m_mgr -> pushStatus( StatusLevel::INFO, ClientStatusType::ACTIVE, "Connected successfully to " + endpoint_id );
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
                    m_mgr -> pushStatus( StatusLevel::INFO, ClientStatusType::CLOSED, "Connection closed" );
                }
            );
            ep -> setOnSendFail(
                [ this, endpoint_id ]( const std::string& s ) {
                    std::stringstream ss;
                    ss << "Failed to send: " << s;
                    m_mgr -> pushStatus( StatusLevel::ERROR, ClientStatusType::MESSAGE_SEND_FAIL, ss.str() + "for " + endpoint_id );
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
};

void WebsocketEndpointManager::shutdownEndpoint(const std::string& endpoint_id) {
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
            endpoint_it->second->stop( false );
            m_endpoints.erase(endpoint_it);
        }
    });
}

void WebsocketEndpointManager::setupEndpoint(const std::string& endpoint_id, 
                                       std::unique_ptr<WebsocketEndpoint> endpoint) 
{
    // Store the endpoint first
    auto& stored_endpoint = m_endpoints[endpoint_id] = std::move(endpoint);

    // Do this directly to not get any issues with race conditions or something. Make this atomic
    stored_endpoint->setOnOpen([this, endpoint_id, endpoint = stored_endpoint.get()]() {
        auto [iter, inserted] = m_endpoint_configs.try_emplace(endpoint_id, m_ioc);
        auto& config = iter->second;
        config.attempting_reconnect = false;
        
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
        
        m_mgr -> pushStatus(StatusLevel::INFO, ClientStatusType::ACTIVE, 
                    "Connected successfully for endpoint " + endpoint_id);
    });

    stored_endpoint->setOnFail([this, endpoint_id](const std::string& reason) {
        handleEndpointFailure(endpoint_id, reason, ClientStatusType::CONNECTION_FAILED);
    });

    stored_endpoint->setOnClose([this, endpoint_id]() {
        handleEndpointClosure(endpoint_id);
    });
    stored_endpoint->setOnMessage([this, endpoint_id](void* data, size_t len) {
        // Here we need to route to all active consumers for this endpoint
        const auto& consumers = m_endpoint_consumers[endpoint_id];
        
        // For each active consumer, we need to send to their input adapter
        PushBatch batch( m_engine -> rootEngine() );  // TODO is this right?
        for (size_t consumer_id = 0; consumer_id < consumers.size(); ++consumer_id) {
            if (consumers[consumer_id]) {
                std::vector<uint8_t> data_copy(static_cast<uint8_t*>(data), 
                                    static_cast<uint8_t*>(data) + len);
                // auto tup = std::tuple<std::string, void*> {endpoint_id, data};
                auto tup = std::tuple<std::string, void*>{endpoint_id, data_copy.data()};
                m_inputAdapters[consumer_id] -> processMessage( std::move(tup), len, &batch );
            }
        }
    });
    stored_endpoint -> run();
};


void WebsocketEndpointManager::handleEndpointFailure(const std::string& endpoint_id, 
                                               const std::string& reason, ClientStatusType status_type) {
    // If there are any active consumers/producers, try to reconnect
    if (!canRemoveEndpoint(endpoint_id)) {
        auto [iter, inserted] = m_endpoint_configs.try_emplace(endpoint_id, m_ioc);
        auto& config = iter->second;
        
        if (!config.attempting_reconnect) {
            config.attempting_reconnect = true;
            
            // Schedule reconnection attempt
            config.reconnect_timer->expires_after(config.reconnect_interval);
            config.reconnect_timer->async_wait([this, endpoint_id](const error_code& ec) {
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
    if ( status_type == ClientStatusType::CLOSED || status_type == ClientStatusType::ACTIVE )
        m_mgr -> pushStatus(StatusLevel::INFO, status_type, ss.str());
    else{
       m_mgr -> pushStatus(StatusLevel::ERROR, status_type, ss.str());
    }
};

void WebsocketEndpointManager::handleEndpointClosure(const std::string& endpoint_id) {
    if (auto config_it = m_endpoint_configs.find(endpoint_id); 
        config_it != m_endpoint_configs.end() && !config_it->second.shutting_down) {
        // Only handle as failure if not intentionally shutting down
        handleEndpointFailure(endpoint_id, "Connection closed", ClientStatusType::CLOSED);
    }
};

void WebsocketEndpointManager::handleConnectionRequest(const Dictionary & properties)
{
    auto endpoint_id = properties.get<std::string>("uri");
    auto caller_id = properties.get<int64_t>("caller_id");
    size_t validated_id = validateCallerId(caller_id);
    autogen::ActionType action = autogen::ActionType::create( properties.get<std::string>("action") );
    auto is_consumer = properties.get<bool>("is_subscribe");
    // Change headers if needed here!
    switch(action.enum_value()) {
        case autogen::ActionType::enum_::CONNECT: {
            auto persistent = properties.get<bool>("persistent");
            if (!persistent){
                WebsocketEndpointManager::setupOneOffConnection(endpoint_id, properties);
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
                    WebsocketEndpointManager::addConsumer(endpoint_id, validated_id);
                } else {
                    WebsocketEndpointManager::addProducer(endpoint_id, validated_id);
                }
                // TODO do we want to update hedaers on the other actions too?
                // This makes a copy for now, maybe make it not do that?
                // Do we want to update if persistent if false too?
                if (is_new_endpoint) {
                    auto endpoint = std::make_unique<WebsocketEndpoint>(m_ioc, properties);
                    endpoint -> updateHeaders(properties);
                    WebsocketEndpointManager::setupEndpoint(endpoint_id, std::move(endpoint));
                }
                else{
                    m_endpoints[endpoint_id]->updateHeaders(properties);
                }
            }
            break;
        }
        
        case csp::autogen::ActionType::enum_::DISCONNECT: {
            // Clear persistence flag for this caller
            if (auto config_it = m_endpoint_configs.find(endpoint_id); config_it != m_endpoint_configs.end()) {
                if (is_consumer) {
                    WebsocketEndpointManager::removeConsumer(endpoint_id, validated_id);
                } else {
                    WebsocketEndpointManager::removeProducer(endpoint_id, validated_id);
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
};


void WebsocketEndpointManager::ensureVectorSize(std::vector<bool>& vec, size_t caller_id) {
    if (vec.size() <= caller_id) {
        vec.resize(caller_id + 1, false);
    }
};

void WebsocketEndpointManager::ensureCallerVectorsSize(size_t caller_id) {
    if (m_consumer_endpoints.size() <= caller_id) {
        m_consumer_endpoints.resize(caller_id + 1);
    }
    if (m_producer_endpoints.size() <= caller_id) {
        m_producer_endpoints.resize(caller_id + 1);
    }
};

void WebsocketEndpointManager::addConsumer(const std::string& endpoint_id, size_t caller_id) {
    ensureVectorSize(m_endpoint_consumers[endpoint_id], caller_id);
    m_endpoint_consumers[endpoint_id][caller_id] = true;
    
    ensureCallerVectorsSize(caller_id);
    m_consumer_endpoints[caller_id].insert(endpoint_id);
};

void WebsocketEndpointManager::addProducer(const std::string& endpoint_id, size_t caller_id) {
    ensureVectorSize(m_endpoint_producers[endpoint_id], caller_id);
    m_endpoint_producers[endpoint_id][caller_id] = true;
    
    ensureCallerVectorsSize(caller_id);
    m_producer_endpoints[caller_id].insert(endpoint_id);
};

bool WebsocketEndpointManager::canRemoveEndpoint(const std::string& endpoint_id) {
    const auto& consumers = m_endpoint_consumers[endpoint_id];
    const auto& producers = m_endpoint_producers[endpoint_id];
    
    // Check if any true values exist in either vector
    return std::none_of(consumers.begin(), consumers.end(), [](bool b) { return b; }) &&
            std::none_of(producers.begin(), producers.end(), [](bool b) { return b; });
};

void WebsocketEndpointManager::removeConsumer(const std::string& endpoint_id, size_t caller_id) {
    auto& consumers = m_endpoint_consumers[endpoint_id];
    if (caller_id < consumers.size()) {
        consumers[caller_id] = false;
    }
    
    if (caller_id < m_consumer_endpoints.size()) {
        m_consumer_endpoints[caller_id].erase(endpoint_id);
    }
};

void WebsocketEndpointManager::removeProducer(const std::string& endpoint_id, size_t caller_id) {
    auto& producers = m_endpoint_producers[endpoint_id];
    if (caller_id < producers.size()) {
        producers[caller_id] = false;
    }
    
    if (caller_id < m_producer_endpoints.size()) {
        m_producer_endpoints[caller_id].erase(endpoint_id);
    }
};


void WebsocketEndpointManager::stop() {
    m_shouldRun=false;
    if( m_dynamic ){
        // Stop the work guard to allow the io_context to complete
        if (m_work_guard )
            m_work_guard.reset();
        
        // Stop all endpoints
        for (auto& [endpoint_id, _] : m_endpoints) {
            shutdownEndpoint(endpoint_id);
            // endpoint->stop();
        }
    }
    else{
        if( m_active ) m_endpoint->stop( false );
    }
    m_ioc.stop();
    m_cv.notify_one();
    if( m_thread ) m_thread->join();
};

PushInputAdapter* WebsocketEndpointManager::getInputAdapter(CspTypePtr & type, PushMode pushMode, const Dictionary & properties)
{   
    if ( m_dynamic ){
        auto caller_id = properties.get<int64_t>("caller_id");
        size_t validated_id = validateCallerId(caller_id);
        auto input_adapter = m_engine -> createOwnedObject<ClientInputAdapter>(
            type,
            pushMode,
            properties,
            m_dynamic   
        );
        m_inputAdapters[validated_id] = input_adapter;
        return m_inputAdapters[validated_id];
    }
    if (m_inputAdapter == nullptr)
    {
        m_inputAdapter = m_engine -> createOwnedObject<ClientInputAdapter>(
            type,
            pushMode,
            properties,
            m_dynamic    
        );
    }
    return m_inputAdapter;
};

OutputAdapter* WebsocketEndpointManager::getOutputAdapter( const Dictionary & properties )
{
    if ( m_dynamic ){
        auto caller_id = properties.get<int64_t>("caller_id");
        size_t validated_id = validateCallerId(caller_id);
        assert(!properties.get<bool>("is_subscribe"));
        assert(m_outputAdapters.size() == validated_id);

        auto output_adapter = m_engine -> createOwnedObject<ClientOutputAdapter>( *m_endpoint, this, validated_id, m_ioc, m_dynamic );
        m_outputAdapters[validated_id] = output_adapter;
        return m_outputAdapters[validated_id];
    }
    // validated_id does not matter here
    if (m_outputAdapter == nullptr) m_outputAdapter = m_engine -> createOwnedObject<ClientOutputAdapter>( *m_endpoint, this, 0, m_ioc, m_dynamic );
    return m_outputAdapter;
};

OutputAdapter * WebsocketEndpointManager::getHeaderUpdateAdapter()
{
    if (m_updateAdapter == nullptr) m_updateAdapter = m_engine -> createOwnedObject<ClientHeaderUpdateOutputAdapter>( m_endpoint -> getProperties() );

    return m_updateAdapter;
};

// TODO Maybe make this const?
OutputAdapter * WebsocketEndpointManager::getConnectionRequestAdapter( const Dictionary & properties )
{
    auto caller_id = properties.get<int64_t>("caller_id");
    auto is_subscribe = properties.get<bool>("is_subscribe");
    
    // Get reference to the hash set for this caller_id
    // std::unordered_set<std::string>& uriSet = target[caller_id];
    
    auto* adapter = m_engine->createOwnedObject<ClientConnectionRequestAdapter>(
        this, m_ioc, is_subscribe, caller_id
    );
    m_connectionRequestAdapters.push_back(adapter);
    
    return adapter;
};

}
