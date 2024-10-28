#include <csp/adapters/websocket/ClientAdapterManager.h>

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
    m_properties( properties ) 
{ };

ClientAdapterManager::~ClientAdapterManager()
{ };

void ClientAdapterManager::start( DateTime starttime, DateTime endtime )
{
    AdapterManager::start( starttime, endtime );

    m_shouldRun = true;
    m_endpoint -> setOnOpen(
        [ this ]() {
            m_active = true;
            m_endpoint -> send( "heyyyy" ); 
            // m_endpoint -> send( value );  // this should be the 'on_connect_payload"
            pushStatus( StatusLevel::INFO, ClientStatusType::ACTIVE, "Connected successfully" );
            // If We dont persist, we should kill here
            // ClientAdapterManager::stopAndJoin();
            // m_shouldRun = false;
            // if (m_active) m_endpoint->stop();
            // if (m_thread) m_thread->join();
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
                // TODO
                PushBatch batch( m_engine -> rootEngine() );
                // here route to correct adapters
                std::string uri =  m_properties.get<std::string>("host") + ":" + m_properties.get<std::string>("port") + m_properties.get<std::string>("route");
                // auto& val = std::tuple(uri, c );
                // m_inputAdapter -> processMessage( c, t, &batch );
                m_inputAdapter -> processMessage( std::tuple(uri, c), t, &batch );
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
            // use something like this:
            //         asio::post(m_ioc, [this]() { processNextRequest(); });
            // to take the connection requests, and process them on the thread running the endpoints.
            m_ioc.reset();
            m_endpoint -> run();
            m_ioc.run();
            // Maybe reset here??
            // How do we add more endpoints here?
            m_active = false;
            if( m_shouldRun ) sleep( m_properties.get<TimeDelta>( "reconnect_interval" ) );
        }
    });
};

// void ClientAdapterManager::stopAndJoin()
// {
//     m_shouldRun = false;
//     if (m_active) m_endpoint->stop();
//     if (m_thread) m_thread->join();
// }

void ClientAdapterManager::processConnectionRequestUpdate( const Dictionary & properties)
{
    return;
    // this is where we do the updates and manage the endpoints
    // We should consider all the different possibilities
    // Also, if dynamic, we need the input adapter to return
    // A wrapped object, or something to signify which
    // endpoint a response is from.
};

void ClientAdapterManager::stop() {
    AdapterManager::stop();

    m_shouldRun=false; 
    if( m_active ) m_endpoint->stop();
    if( m_thread ) m_thread->join();
};

PushInputAdapter* ClientAdapterManager::getInputAdapter(CspTypePtr & type, PushMode pushMode, const Dictionary & properties)
{
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

OutputAdapter* ClientAdapterManager::getOutputAdapter()
{
    if (m_outputAdapter == nullptr) m_outputAdapter = m_engine -> createOwnedObject<ClientOutputAdapter>(*m_endpoint);

    return m_outputAdapter;
}

OutputAdapter * ClientAdapterManager::getHeaderUpdateAdapter()
{
    if (m_updateAdapter == nullptr) m_updateAdapter = m_engine -> createOwnedObject<ClientHeaderUpdateOutputAdapter>( m_endpoint -> getProperties() );

    return m_updateAdapter;
}

OutputAdapter * ClientAdapterManager::getConnectionRequestAdapter( const Dictionary & properties )
{
    auto caller_id = properties.get<uint64_t>("caller_id");
    auto is_subscribe = properties.get<bool>("is_subscribe");
    
    // Select the appropriate vector based on is_subscribe flag
    std::vector<std::unordered_set<std::string>>& target = 
        is_subscribe ? m_subscribeFromUri : m_sendToUri;
    
    // If caller_id is beyond current vector size, add new sets until we reach it
    while (caller_id >= target.size()) {
        target.emplace_back(); 
    }
    
    // Get reference to the hash set for this caller_id
    // std::unordered_set<std::string>& uriSet = target[caller_id];
    
    auto* adapter = m_engine->createOwnedObject<ClientConnectionRequestAdapter>(
        m_endpoint->getProperties(), caller_id, this, m_ioc
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
