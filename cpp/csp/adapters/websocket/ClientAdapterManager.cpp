#include <csp/adapters/websocket/ClientAdapterManager.h>
#include <iostream>

// namespace csp {

// INIT_CSP_ENUM( adapters::websocket::ClientStatusType,
//                "ACTIVE",
//                "GENERIC_ERROR",
//                "CONNECTION_FAILED",
//                "CLOSED",
//                "MESSAGE_SEND_FAIL",
// );

// }

// With TLS
namespace csp::adapters::websocket {

ClientAdapterManager::ClientAdapterManager( Engine* engine, const Dictionary & properties ) 
: AdapterManager( engine ), 
    // m_ioc(),
    // m_active( false ), 
    // m_shouldRun( false ), 
    // m_endpoint(!properties.get<bool>("dynamic") ? 
    //     std::make_unique<WebsocketEndpoint>( m_ioc, properties ) : 
    //     nullptr),
    // // m_endpoint( std::make_unique<WebsocketEndpoint>( m_ioc, properties ) ),
    // m_inputAdapter( nullptr ), 
    // m_outputAdapter( nullptr ),
    // m_updateAdapter( nullptr ),
    // m_thread( nullptr ), 
    m_properties( properties )
    // m_work_guard(properties.get<bool>("dynamic") ? 
    //     std::make_optional(boost::asio::make_work_guard(m_ioc)) : 
    //     std::nullopt),
    // m_dynamic( properties.get<bool>("dynamic") )
{ }

ClientAdapterManager::~ClientAdapterManager()
{ }

WebsocketEndpointManager* ClientAdapterManager::getWebsocketManager(){
    if( m_endpointManager == nullptr )
        return nullptr;
    return m_endpointManager.get();
}

void ClientAdapterManager::start(DateTime starttime, DateTime endtime) {
    AdapterManager::start(starttime, endtime);
    if (m_endpointManager != nullptr)
        m_endpointManager -> start(starttime, endtime);
}

void ClientAdapterManager::stop() {
    AdapterManager::stop();
    if (m_endpointManager != nullptr)
        m_endpointManager -> stop();
}

PushInputAdapter* ClientAdapterManager::getInputAdapter(CspTypePtr & type, PushMode pushMode, const Dictionary & properties)
{   
    if (m_endpointManager == nullptr)
        m_endpointManager = std::make_unique<WebsocketEndpointManager>(this, m_properties, m_engine);
    return m_endpointManager -> getInputAdapter( type, pushMode, properties );
}

OutputAdapter* ClientAdapterManager::getOutputAdapter( const Dictionary & properties )
{
    if (m_endpointManager == nullptr)
        m_endpointManager = std::make_unique<WebsocketEndpointManager>(this, m_properties, m_engine);
    return m_endpointManager -> getOutputAdapter( properties );
}

OutputAdapter * ClientAdapterManager::getHeaderUpdateAdapter()
{
   if (m_endpointManager == nullptr)
        m_endpointManager = std::make_unique<WebsocketEndpointManager>(this, m_properties, m_engine);
    return m_endpointManager -> getHeaderUpdateAdapter();
}

// TODO Maybe make this const?
OutputAdapter * ClientAdapterManager::getConnectionRequestAdapter( const Dictionary & properties )
{
    if (m_endpointManager == nullptr)
        m_endpointManager = std::make_unique<WebsocketEndpointManager>(this, m_properties, m_engine);
    return m_endpointManager -> getConnectionRequestAdapter( properties );
}

DateTime ClientAdapterManager::processNextSimTimeSlice( DateTime time )
{
    // no sim data
    return DateTime::NONE();
}

}
