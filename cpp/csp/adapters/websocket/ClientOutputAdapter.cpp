#include <csp/adapters/websocket/ClientOutputAdapter.h>

namespace csp::adapters::websocket {

ClientOutputAdapter::ClientOutputAdapter(
    Engine * engine,
    WebsocketEndpoint& endpoint,
    WebsocketEndpointManager * websocketManager,
    size_t caller_id,
    net::io_context& ioc,
    bool dynamic
) : OutputAdapter( engine ), 
    m_endpoint( endpoint ), 
    m_websocketManager( websocketManager ),
    m_callerId( caller_id ),
    m_ioc( ioc ),
    m_dynamic( dynamic )
{ };

void ClientOutputAdapter::executeImpl()
{
    // TODO Add here picking the right endpoints to send to
    // Based on the caller id
    const std::string & value = input() -> lastValueTyped<std::string>();
    if( m_dynamic ){
        boost::asio::post(m_ioc, [this, value=value]() {
            // something something lifetime? Not sure
            m_websocketManager->send(value, m_callerId);
        });
    }
    else{
        m_endpoint.send( value );
    }
};

}